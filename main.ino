#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h> 
#include <ArduinoJson.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>
#include <UniversalTelegramBot.h>
#include <time.h> 

// --- Configuração de Rede ---
const char* ssid = "Didi Moco"; 
const char* password = "alanabanana";

// --- Configuração do Web Service (Render) ---
const char* serverUrl = "https://servidor-cuida.onrender.com/api/reportar_evento";

// --- Configurações Telegram (REINTEGRADAS) ---
#define BOTtoken "8273549872:AAGP4EzGTU58K5Ks75SbGIU8-8_0HLxM74M"
#define CHAT_ID "8044913511"

// --- Clientes ---
WiFiClientSecure client;
UniversalTelegramBot bot(BOTtoken, client);

// --- Pinos ---
#define LED_ALERTA_PIN 4
#define LED_WIFI_PIN 5
#define BUZZER_1_PIN 10
#define BUZZER_2_PIN 7

// --- Parâmetros de Detecção ---
#define FALL_THRESHOLD 40.0
#define DEBOUNCE_TIME 3000

Adafruit_MPU6050 mpu;
bool fallDetected = false;
unsigned long lastFallTime = 0;
bool wifiConnectedFirstTime = false;
unsigned long lastBlinkTime = 0;
bool alertState = false; 

// -----------------------------------------------------------
//      ÁRVORE BINÁRIA DE CLASSIFICAÇÃO (REINTEGRADA)
// -----------------------------------------------------------
struct Node {
    float threshold;
    String label;
    Node* left;
    Node* right;
};

Node* root = nullptr;

Node* createNode(float threshold, String label) {
    Node* n = new Node();
    n->threshold = threshold;
    n->label = label;
    n->left = nullptr;
    n->right = nullptr;
    return n;
}

void buildClassificationTree() {
    // Nó raiz
    root = createNode(40, "Queda Detectada");
    // Lado esquerdo (menos grave)
    root->left = createNode(25, "Movimento Brusco");
    root->left->left = createNode(15, "Impacto Leve");
    root->left->left->left = createNode(10, "Movimento Leve");
    root->left->right = createNode(30, "Quase Queda");
    // Lado direito (mais grave)
    root->right = createNode(60, "Queda Grave");
}

String classifyAcceleration(Node* node, float value) {
    if (node == nullptr)
        return "Sem classificação";
    if (value == node->threshold)
        return node->label;
    if (value < node->threshold) {
        if (node->left == nullptr) return node->label;
        return classifyAcceleration(node->left, value);
    }
    else {
        if (node->right == nullptr) return node->label;
        return classifyAcceleration(node->right, value);
    }
}

// -----------------------------------------------------------
//      HISTÓRICO DE QUEDAS GRAVES (REINTEGRADO)
// -----------------------------------------------------------
#define MAX_HISTORICO 20
struct EventoQueda {
    time_t timestamp;
    float aceleracao;
    String classe;
};

EventoQueda historico[MAX_HISTORICO];
int historicoCount = 0;

void registrarQuedaGrave(float aceleracao, String classe) {
    if (historicoCount >= MAX_HISTORICO) {
        for (int i = 1; i < MAX_HISTORICO; i++) {
            historico[i - 1] = historico[i];
        }
        historicoCount = MAX_HISTORICO - 1;
    }
    historico[historicoCount].timestamp = time(nullptr);
    historico[historicoCount].aceleracao = aceleracao;
    historico[historicoCount].classe = classe;
    historicoCount++;
    Serial.println("Queda grave registrada no histórico.");
}

void imprimirHistorico() {
    Serial.println("\n======= HISTÓRICO DE QUEDAS GRAVES =======");
    if (historicoCount == 0) {
        Serial.println("Nenhuma queda grave registrada.");
        return;
    }
    for (int i = 0; i < historicoCount; i++) {
        struct tm* t = localtime(&historico[i].timestamp);
        Serial.print(i + 1);
        Serial.print(") Data: ");
        Serial.printf("%02d/%02d/%04d ", t->tm_mday, t->tm_mon + 1, t->tm_year + 1900);
        Serial.printf("Horario: %02d:%02d:%02d", t->tm_hour, t->tm_min, t->tm_sec);
        Serial.print(" | Aceleração: ");
        Serial.print(historico[i].aceleracao);
        Serial.print(" | Classe: ");
        Serial.println(historico[i].classe);
    }
    Serial.println("==========================================\n");
}

// -----------------------------------------------------------
//      FUNÇÕES DE ALERTA (WEB SERVICE E TELEGRAM)
// -----------------------------------------------------------

// --- Função de Envio Telegram (REINTEGRADA) ---
void enviarMensagemTelegram(String texto) {
    if (WiFi.status() == WL_CONNECTED) {
        bot.sendMessage(CHAT_ID, texto, "Markdown"); // Usando Markdown para negrito
        Serial.println("Mensagem enviada ao Telegram.");
    } else {
        Serial.println("Falha ao enviar mensagem Telegram: sem conexão Wi-Fi.");
    }
}

void sendAlertToApi(float accMag, String classe) { 
    if (WiFi.status() == WL_CONNECTED) { 
        HTTPClient http;
        if (http.begin(client, serverUrl)) { 
            Serial.println("[sendAlertToApi] Conexão HTTP iniciada.");
            http.addHeader("Content-Type", "application/json");
            StaticJsonDocument<200> jsonDoc;

            jsonDoc["tipo_evento"] = "queda"; 
            jsonDoc["latitude"] = "-25.4320";  
            jsonDoc["longitude"] = "-49.2770";
            
            char accBuffer[20];
            snprintf(accBuffer, sizeof(accBuffer), "%.2fg", accMag); 
            jsonDoc["aceleracao"] = accBuffer;

            String jsonPayload;
            serializeJson(jsonDoc, jsonPayload);

            Serial.println("Enviando alerta para o servidor Web...");
            Serial.println(jsonPayload);
            int httpResponseCode = http.POST(jsonPayload);

            if (httpResponseCode == 200) {
                Serial.println(">>> DADOS SALVOS NO BANCO DE DADOS (WEB SERVICE)! <<<");
            } else {
                Serial.print("Erro ao enviar alerta (Web Service). Código: ");
                Serial.println(httpResponseCode);
                Serial.printf("Erro: %s\n", http.errorToString(httpResponseCode).c_str());
            }
            http.end();
        } else {
            Serial.println("[sendAlertToApi] Falha ao iniciar conexão HTTP.");
        }
    } else {
        Serial.println("[sendAlertToApi] Wi-Fi desconectado.");
    }
}

// -----------------------------------------------------------
//      FUNÇÕES PRINCIPAIS DO SISTEMA
// -----------------------------------------------------------

void setupWifi();
void setupSensor();
void checkForFall();
void triggerAlert(float accMag, String classe); 
void sinalizarAlerta();

void setup() {
    Serial.begin(115200);
    while (!Serial) { delay(10); }

    Serial.println("\nIniciando Sistema de Detecção (Completo: WebService + Telegram)");

    pinMode(LED_ALERTA_PIN, OUTPUT);
    pinMode(LED_WIFI_PIN, OUTPUT);
    pinMode(BUZZER_1_PIN, OUTPUT);
    pinMode(BUZZER_2_PIN, OUTPUT);
    
    digitalWrite(LED_ALERTA_PIN, LOW);
    digitalWrite(LED_WIFI_PIN, LOW);
    digitalWrite(BUZZER_1_PIN, LOW);
    digitalWrite(BUZZER_2_PIN, LOW);

    client.setInsecure();

    setupWifi();
    setupSensor();

    buildClassificationTree();
    Serial.println("\nÁrvore binária de classificação carregada.");
    Serial.println("Digite 'h' para histórico, 'r' para resetar alarme.\n");
}

void loop() {
    if (Serial.available()) {
        char cmd = Serial.read();
        if (cmd == 'h' || cmd == 'H') {
            imprimirHistorico();
        }
        if (cmd == 'r' || cmd == 'R') {
            if (fallDetected) {
                Serial.println("\n[SISTEMA] Alarme resetado manualmente.");
                fallDetected = false;
                digitalWrite(LED_ALERTA_PIN, LOW);
                digitalWrite(BUZZER_1_PIN, LOW);
                digitalWrite(BUZZER_2_PIN, LOW);
                alertState = false; 
            } else {
                Serial.println("\n[SISTEMA] Nenhum alarme ativo para resetar.");
            }
        }
    }

    if (fallDetected) {
        sinalizarAlerta(); 
    } else {
        checkForFall();
    }

    if (WiFi.status() != WL_CONNECTED) {
        if (wifiConnectedFirstTime) {
            Serial.println("\nConexão Wi-Fi perdida! O sistema tentará reconectar automaticamente.");
            wifiConnectedFirstTime = false; 
        }
        digitalWrite(LED_WIFI_PIN, (millis() / 500) % 2); 
    } else {
        if (!wifiConnectedFirstTime) {
            digitalWrite(LED_WIFI_PIN, HIGH); 
            Serial.println("\n---------------------------------------------");
            Serial.println("Wi-Fi conectado com sucesso!");
            Serial.print("Endereço IP: "); Serial.println(WiFi.localIP());
            Serial.println("---------------------------------------------");
            wifiConnectedFirstTime = true; 
        }
        digitalWrite(LED_WIFI_PIN, HIGH);
    }
}

void sinalizarAlerta() {
    unsigned long currentTime = millis();
    if (currentTime - lastBlinkTime > 200) { 
        lastBlinkTime = currentTime;
        alertState = !alertState; 

        digitalWrite(LED_ALERTA_PIN, alertState);
        digitalWrite(BUZZER_1_PIN, alertState);
        digitalWrite(BUZZER_2_PIN, alertState);
    }
}

void setupWifi() {
    Serial.println("\nIniciando conexão Wi-Fi em segundo plano...");
    Serial.print("Conectando à rede: ");
    Serial.println(ssid);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true); 
    WiFi.begin(ssid, password);
    // Configura o cliente NTP para pegar a hora certa para o histórico
    configTime(0, 0, "pool.ntp.org");
}

void setupSensor() {
    if (!mpu.begin()) {
        Serial.println("Falha ao encontrar o sensor MPU6050. Verifique a conexão!");
        while (1) {
            digitalWrite(LED_ALERTA_PIN, HIGH); delay(100);
            digitalWrite(LED_ALERTA_PIN, LOW); delay(100);
        }
    }
    Serial.println("Sensor MPU6050 inicializado com sucesso!");
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
}

void checkForFall() {
    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);
    float accelerationMagnitude = sqrt(pow(a.acceleration.x, 2) + pow(a.acceleration.y, 2) + pow(a.acceleration.z, 2));

    String classe = classifyAcceleration(root, accelerationMagnitude);

    Serial.print("Aceleração: ");
    Serial.print(accelerationMagnitude);
    Serial.print(" -> Classificação: ");
    Serial.println(classe);

    if (classe == "Queda Grave") {
        registrarQuedaGrave(accelerationMagnitude, classe);
    }

    if (accelerationMagnitude > FALL_THRESHOLD) {
        if (millis() - lastFallTime > DEBOUNCE_TIME) {
            triggerAlert(accelerationMagnitude, classe); 
            lastFallTime = millis();
        }
    }
}

void triggerAlert(float accMag, String classe) { 
    fallDetected = true;
    lastBlinkTime = millis();
    alertState = true; 

    Serial.println("\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    Serial.println("!!!       ALERTA DE QUEDA               !!!");
    Serial.print("!!!       CLASSE: "); Serial.println(classe);
    Serial.println("!!!   (Digite 'r' para resetar)         !!!");
    Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");

    sendAlertToApi(accMag, classe); 

    String msg = "⚠️ *ALERTA DE QUEDA DETECTADO!*\n*Classificação:* " + classe + "\n*Valor (Acel):* " + String(accMag, 2) + "g";
    enviarMensagemTelegram(msg);
}