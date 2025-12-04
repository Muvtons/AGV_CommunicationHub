#include "AGVCoreNetwork.h"
#include "AGVCoreNetwork_Resources.h"

using namespace AGVCoreNetworkLib;

AGVCoreNetwork agvNetwork; // Global instance

// Forward declarations
void webSocketEventHandler(uint8_t num, WStype_t type, uint8_t * payload, size_t length);

// WebSocket event wrapper
void webSocketEventHandler(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  agvNetwork.webSocketEvent(num, type, payload, length);
}

void AGVCoreNetwork::begin(const char* deviceName, const char* adminUser, const char* adminPass) {
  Serial.println("\n[AGVNET] Initializing AGV Core Network System...");
  
  // Initialize mutex for thread safety
  mutex = xSemaphoreCreateMutex();
  
  // Store configuration
  this->mdnsName = deviceName;
  this->admin_username = adminUser;
  this->admin_password = adminPass;
  
  // Initialize hardware
  Serial.begin(115200);
  delay(500);
  
  // Initialize preferences
  preferences.begin("agvnet", false);
  stored_ssid = preferences.getString("ssid", "");
  stored_password = preferences.getString("password", "");
  preferences.end();
  
  // Setup WiFi based on stored credentials
  setupWiFi();
  
  // Start Core 0 task (handles all communication)
  xTaskCreatePinnedToCore(
    [](void* param) {
      AGVCoreNetwork* net = (AGVCoreNetwork*)param;
      net->core0Task(NULL);
    },
    "AGVNetCore0",
    16384,  // Stack size
    this,
    1,      // Priority
    &core0TaskHandle,
    0       // Core 0
  );
  
  Serial.println("[AGVNET] ✅ Network System started on Core 0");
}

void AGVCoreNetwork::setCommandCallback(CommandCallback callback) {
  if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) == pdPASS) {
    this->commandCallback = callback;
    xSemaphoreGive(mutex);
    Serial.println("[AGVNET] Command callback registered");
  }
}

void AGVCoreNetwork::sendStatus(const char* status) {
  if (!status || strlen(status) == 0) return;
  
  if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) == pdPASS) {
    if (!isAPMode && webSocket) {
      webSocket->broadcastTXT(status);
    }
    xSemaphoreGive(mutex);
  }
  Serial.printf("[STATUS] %s\n", status);
}

void AGVCoreNetwork::broadcastEmergency(const char* message) {
  if (!message || strlen(message) == 0) return;
  
  if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) == pdPASS) {
    if (!isAPMode && webSocket) {
      webSocket->broadcastTXT(String("EMERGENCY: ") + message);
    }
    xSemaphoreGive(mutex);
  }
  Serial.printf("!!! EMERGENCY: %s\n", message);
}

void AGVCoreNetwork::cleanupServer() {
  if (server) {
    delete server;
    server = nullptr;
  }
}

void AGVCoreNetwork::cleanupWebSocket() {
  if (webSocket) {
    delete webSocket;
    webSocket = nullptr;
  }
}

void AGVCoreNetwork::cleanupDNSServer() {
  if (dnsServer) {
    dnsServer->stop();
    delete dnsServer;
    dnsServer = nullptr;
  }
}

void AGVCoreNetwork::setupWiFi() {
  if (stored_ssid.length() > 0) {
    Serial.println("[AGVNET] Found saved WiFi credentials, attempting connection...");
    startStationMode();
  } else {
    Serial.println("[AGVNET] No saved credentials, starting AP mode...");
    startAPMode();
  }
}

void AGVCoreNetwork::startAPMode() {
  Serial.println("\n[AGVNET] 📡 Starting Access Point Mode");
  
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_password);
  
  IPAddress IP = WiFi.softAPIP();
  Serial.printf("[AGVNET] AP IP: %d.%d.%d.%d\n", IP[0], IP[1], IP[2], IP[3]);
  Serial.println("[AGVNET] Connect to '" + String(ap_ssid) + "' network");
  Serial.println("[AGVNET] Open any browser for setup wizard");
  
  delay(100); // Give AP time to start
  
  // Create DNS server for captive portal
  dnsServer = new DNSServer();
  dnsServer->start(53, "*", WiFi.softAPIP());
  
  isAPMode = true;
  
  // Setup web server - CRITICAL ORDER FOR CAPTIVE PORTAL
  server = new WebServer(80);
  
  // Setup routes for AP mode - IMPORTANT ORDER (onNotFound FIRST!)
  server->onNotFound([this](){ this->handleNotFound(); });
  server->on("/", HTTP_GET, [this](){ this->handleRootRedirect(); });
  server->on("/setup", HTTP_GET, [this](){ this->handleWiFiSetup(); });
  server->on("/scan", HTTP_GET, [this](){ this->handleScan(); });
  server->on("/savewifi", HTTP_POST, [this](){ this->handleSaveWiFi(); });
  
  // Common captive portal detection URLs
  server->on("/generate_204", HTTP_GET, [this](){ this->handleRootRedirect(); });
  server->on("/fwlink", HTTP_GET, [this](){ this->handleRootRedirect(); });
  server->on("/redirect", HTTP_GET, [this](){ this->handleRootRedirect(); });
  server->on("/hotspot-detect.html", HTTP_GET, [this](){ this->handleRootRedirect(); });
  server->on("/connectivity-check.html", HTTP_GET, [this](){ this->handleRootRedirect(); });
  server->on("/check_network_status.txt", HTTP_GET, [this](){ this->handleRootRedirect(); });
  server->on("/ncsi.txt", HTTP_GET, [this](){ this->handleRootRedirect(); });
  
  server->begin();
  Serial.println("[AGVNET] ✅ AP Mode Web Server Started");
  Serial.println("[AGVNET] ✅ Captive portal enabled - all requests redirected to setup");
}

void AGVCoreNetwork::startStationMode() {
  Serial.println("\n[AGVNET] 🌐 Starting Station Mode");
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(stored_ssid.c_str(), stored_password.c_str());
  
  Serial.printf("[AGVNET] Connecting to: %s\n", stored_ssid.c_str());
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[AGVNET] ✅ WiFi Connected!");
    Serial.printf("[AGVNET] IP Address: %s\n", WiFi.localIP().toString().c_str());
    
    // Start mDNS
    if (MDNS.begin(mdnsName)) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("[AGVNET] ✅ mDNS started: http://%s.local\n", mdnsName);
    }
    
    isAPMode = false;
    
    // Setup web server
    server = new WebServer(80);
    webSocket = new WebSocketsServer(81);
    webSocket->begin();
    webSocket->onEvent(webSocketEventHandler);
    
    setupRoutes();
    server->begin();
    
    Serial.println("[AGVNET] ✅ Station Mode Web Server Started");
    Serial.println("[AGVNET] ✅ WebSocket Server Started (Port 81)");
    
  } else {
    Serial.println("\n[AGVNET] ❌ WiFi connection failed");
    Serial.println("[AGVNET] Falling back to AP mode...");
    startAPMode();
  }
}

void AGVCoreNetwork::setupRoutes() {
  if (!server) return;
  
  if (isAPMode) {
    // This should never be called in AP mode - routes are set in startAPMode()
    return;
  } else {
    // Station Mode routes (control interface)
    server->on("/", HTTP_GET, [this](){ this->handleRoot(); });
    server->on("/login", HTTP_POST, [this](){ this->handleLogin(); });
    server->on("/dashboard", HTTP_GET, [this](){ this->handleDashboard(); });
    server->onNotFound([this](){ this->handleNotFound(); });
  }
}

void AGVCoreNetwork::core0Task(void *parameter) {
  Serial.println("[CORE0] AGV Network task started on Core 0");
  
  while(1) {
    if (isAPMode && dnsServer) {
      dnsServer->processNextRequest();
    }
    
    if (server) {
      server->handleClient();
    }
    
    if (!isAPMode) {
      if (webSocket) {
        webSocket->loop();
      }
      processSerialInput();
    }
    
    delay(1);  // Short delay for responsiveness
  }
}

void AGVCoreNetwork::processSerialInput() {
  static char serialBuffer[64];
  static int bufferIndex = 0;
  
  while (Serial.available() > 0) {
    char c = Serial.read();
    
    if (c == '\n' || c == '\r') {
      if (bufferIndex > 0) {
        serialBuffer[bufferIndex] = '\0';
        String cmd = String(serialBuffer);
        cmd.trim();
        
        if (cmd.length() > 0) {
          Serial.printf("\n[SERIAL] Command received: '%s'\n", cmd.c_str());
          
          // Determine priority (emergency commands get high priority)
          uint8_t priority = 0;
          if (cmd.equalsIgnoreCase("STOP") || cmd.equalsIgnoreCase("ABORT") || 
              cmd.equalsIgnoreCase("EMERGENCY")) {
            priority = 1;
          }
          
          // Process command with mutex protection
          if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) == pdPASS) {
            processCommandUnsafe(serialBuffer, 1); // source=1 (serial)
            xSemaphoreGive(mutex);
          }
          
          // Echo back to serial
          Serial.printf("[SERIAL] Executed: %s\n", serialBuffer);
          
          // Broadcast to web clients
          if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) == pdPASS) {
            if (!isAPMode && webSocket) {
              webSocket->broadcastTXT(String("SERIAL: ") + serialBuffer);
            }
            xSemaphoreGive(mutex);
          }
        }
        
        bufferIndex = 0;
      }
    } else if (bufferIndex < sizeof(serialBuffer) - 1) {
      serialBuffer[bufferIndex++] = c;
    }
    
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

String AGVCoreNetwork::getSessionToken() {
  String token = "";
  for(int i = 0; i < 32; i++) {
    token += String(random(0, 16), HEX);
  }
  return token;
}

// Command processing without mutex (called from within mutex-protected context)
void AGVCoreNetwork::processCommandUnsafe(const char* cmd, uint8_t source) {
  // Determine priority
  uint8_t priority = 0;
  String cmdStr = String(cmd);
  if (cmdStr.equalsIgnoreCase("STOP") || cmdStr.equalsIgnoreCase("ABORT") || 
      cmdStr.startsWith("PATH:") || cmdStr.equalsIgnoreCase("START")) {
    priority = 1;
  }
  
  // Send to command callback if registered
  if (commandCallback) {
    commandCallback(cmd, source, priority);
  }
}

// WebSocket event handler - FIXED IMPLEMENTATION
void AGVCoreNetwork::webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) != pdPASS) return;
  
  switch(type) {
    case WStype_DISCONNECTED:
      Serial.printf("[WS] Client #%u disconnected\n", num);
      break;
      
    case WStype_CONNECTED:
      {
        if (webSocket) {
          IPAddress ip = webSocket->remoteIP(num);
          Serial.printf("[WS] Client #%u connected from %d.%d.%d.%d\n", 
                       num, ip[0], ip[1], ip[2], ip[3]);
          webSocket->sendTXT(num, "AGV Connected - Ready for commands");
        }
      }
      break;
      
    case WStype_TEXT:
      {
        char cmd[65];
        snprintf(cmd, sizeof(cmd), "%.*s", length, (char*)payload);
        
        Serial.printf("\n[WS] Command received from client #%u: '%s'\n", num, cmd);
        
        // Process command immediately
        processCommandUnsafe(cmd, 0);  // 0 = WebSocket source
        
        // Echo back to sender
        if (webSocket) {
          String response = "Received: " + String(cmd);
          webSocket->sendTXT(num, response);
          
          // Broadcast to all other clients
          String broadcastMsg = "CLIENT: " + String(cmd);
          webSocket->broadcastTXT(broadcastMsg);
        }
      }
      break;
  }
  
  xSemaphoreGive(mutex);
}

// Web route handlers (SPECIAL HANDLING FOR AP MODE)
void AGVCoreNetwork::handleRoot() {
  if (isAPMode) {
    // In AP mode, no mutex protection needed for redirect
    server->sendHeader("Location", "/setup", true);
    server->send(302, "text/plain", "");
  } else {
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) != pdPASS) return;
    server->send_P(200, "text/html", loginPage);
    xSemaphoreGive(mutex);
  }
}

void AGVCoreNetwork::handleRootRedirect() {
  // In AP mode, no mutex protection needed for redirect
  server->sendHeader("Location", "/setup", true);
  server->send(302, "text/plain", "");
}

void AGVCoreNetwork::handleLogin() {
  if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) != pdPASS) return;
  
  if (!server || server->method() != HTTP_POST) {
    xSemaphoreGive(mutex);
    return;
  }
  
  String body = server->arg("plain");
  
  int userStart = body.indexOf("\"username\":\"") + 12;
  int userEnd = body.indexOf("\"", userStart);
  String username = "";
  if (userStart > 12 && userEnd > userStart && userEnd < (int)body.length()) {
    username = body.substring(userStart, userEnd);
  }
  
  int passStart = body.indexOf("\"password\":\"") + 12;
  int passEnd = body.indexOf("\"", passStart);
  String password = "";
  if (passStart > 12 && passEnd > passStart && passEnd < (int)body.length()) {
    password = body.substring(passStart, passEnd);
  }
  
  Serial.printf("\n[AUTH] Login attempt: '%s'\n", username.c_str());
  
  if (username == admin_username && password == admin_password) {
    sessionToken = getSessionToken();
    String response = "{\"success\":true,\"token\":\"" + sessionToken + "\"}";
    server->send(200, "application/json", response);
    Serial.println("[AUTH] ✅ Login successful");
  } else {
    server->send(200, "application/json", "{\"success\":false}");
    Serial.println("[AUTH] ❌ Login failed");
  }
  
  xSemaphoreGive(mutex);
}

void AGVCoreNetwork::handleDashboard() {
  if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) != pdPASS) return;
  
  if (server) {
    server->send_P(200, "text/html", mainPage);
  }
  
  xSemaphoreGive(mutex);
}

void AGVCoreNetwork::handleWiFiSetup() {
  // In AP mode, no mutex protection needed for page serving
  if (server) {
    server->send_P(200, "text/html", wifiSetupPage);
  }
}

void AGVCoreNetwork::handleScan() {
  if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) != pdPASS) return;
  
  if (!server) {
    xSemaphoreGive(mutex);
    return;
  }
  
  Serial.println("[WIFI] Scanning networks...");
  int n = WiFi.scanNetworks();
  String json = "[";
  
  for (int i = 0; i < n; i++) {
    if (i > 0) json += ",";
    json += "{";
    json += "\"ssid\":\"" + WiFi.SSID(i) + "\",";
    json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
    json += "\"secured\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false");
    json += "}";
  }
  
  json += "]";
  server->send(200, "application/json", json);
  Serial.printf("[WIFI] Found %d networks\n", n);
  
  xSemaphoreGive(mutex);
}

void AGVCoreNetwork::handleSaveWiFi() {
  if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) != pdPASS) return;
  
  if (!server || server->method() != HTTP_POST) {
    xSemaphoreGive(mutex);
    return;
  }
  
  String body = server->arg("plain");
  
  int ssidStart = body.indexOf("\"ssid\":\"") + 8;
  int ssidEnd = body.indexOf("\"", ssidStart);
  String ssid = "";
  if (ssidStart > 8 && ssidEnd > ssidStart && ssidEnd < (int)body.length()) {
    ssid = body.substring(ssidStart, ssidEnd);
  }
  
  int passStart = body.indexOf("\"password\":\"") + 12;
  int passEnd = body.indexOf("\"", passStart);
  String password = "";
  if (passStart > 12 && passEnd > passStart && passEnd < (int)body.length()) {
    password = body.substring(passStart, passEnd);
  }
  
  Serial.printf("\n[WIFI] Saving credentials: '%s'\n", ssid.c_str());
  
  // Save to preferences
  preferences.begin("agvnet", false);
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  preferences.end();
  
  server->send(200, "application/json", "{\"success\":true}");
  
  xSemaphoreGive(mutex);
  
  Serial.println("[WIFI] ✅ Credentials saved. Restarting...");
  delay(1000);
  ESP.restart();
}

void AGVCoreNetwork::handleCaptivePortal() {
  // This should not be called directly in AP mode
  String header = "HTTP/1.1 302 Found\r\n";
  header += "Location: http://192.168.4.1/setup\r\n";
  header += "Cache-Control: no-cache\r\n";
  header += "Connection: close\r\n";
  header += "\r\n";
  
  server->client().write(header.c_str(), header.length());
  server->client().stop();
}

void AGVCoreNetwork::handleNotFound() {
  if (isAPMode) {
    // In AP mode, redirect everything to setup - NO MUTEX NEEDED
    String header = "HTTP/1.1 302 Found\r\n";
    header += "Location: http://192.168.4.1/setup\r\n";
    header += "Cache-Control: no-cache\r\n";
    header += "Connection: close\r\n";
    header += "\r\n";
    
    server->client().write(header.c_str(), header.length());
    server->client().stop();
  } else {
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) != pdPASS) return;
    server->send(404, "text/plain", "File Not Found");
    xSemaphoreGive(mutex);
  }
}