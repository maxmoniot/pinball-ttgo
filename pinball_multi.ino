/*
 * NEON PINBALL MULTIPLAYER - TTGO T-Display ESP32
 * Version Multi-joueurs (2-10 joueurs)
 * 
 * Chaque ESP32 peut rejoindre la partie à tout moment!
 * Les balles vont aléatoirement chez l'un des adversaires.
 */

#include <TFT_eSPI.h>
#include <SPI.h>
#include <esp_now.h>
#include <WiFi.h>
#include "esp_sleep.h"

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite buffer = TFT_eSprite(&tft);

// Boutons
#define BTN_LEFT 0
#define BTN_RIGHT 35

// Interrupteur ON/OFF (deep sleep)
#define POWER_SWITCH_PIN 13

// === OPTION DEEP SLEEP ===
// Mettre à true pour activer le deep sleep avec l'interrupteur sur GPIO 13
// Mettre à false pour désactiver (l'ESP reste toujours allumé)
#define ENABLE_DEEP_SLEEP false

// Backlight de l'écran (TTGO T-Display)
#ifndef TFT_BL
#define TFT_BL 4
#endif

// Dimensions écran
#define SCREEN_W 135
#define SCREEN_H 240

// Zone de jeu
#define SCORE_BAR_H 24
#define GAME_TOP SCORE_BAR_H
#define GAME_H (SCREEN_H - SCORE_BAR_H)

// === PALETTE DE COULEURS ===
#define COL_BG          0x0821
#define COL_BG_DARK     0x0000
#define COL_BALL_MINE   0x07FF  // Cyan
#define COL_BALL_ENEMY  0xF81F  // Magenta
#define COL_BALL_GLOW   0xC618
#define COL_FLIPPER     0x07FF
#define COL_FLIPPER_DIM 0x0492
#define COL_BUMPER_1    0xF81F
#define COL_BUMPER_2    0xFFE0
#define COL_BUMPER_3    0x07E0
#define COL_TARGET      0xFD20
#define COL_WALL        0x2945
#define COL_WALL_LIGHT  0x4A69
#define COL_SCORE_BG    0x1082
#define COL_SCORE_TXT   0x07FF
#define COL_NEON_PINK   0xF81F
#define COL_GOLD        0xFE60
#define COL_CONNECTED   0x07E0
#define COL_SEARCHING   0xFFE0

// Palette de couleurs pour les joueurs (10 couleurs distinctes)
const uint16_t PLAYER_COLORS[10] = {
  0x07FF,  // Cyan
  0xF81F,  // Magenta
  0x07E0,  // Vert
  0xFFE0,  // Jaune
  0xF800,  // Rouge
  0x001F,  // Bleu
  0xFBE0,  // Orange
  0x7BEF,  // Gris clair
  0xF81F,  // Rose
  0x07FF   // Cyan clair
};

// Physique
#define GRAVITY 0.12f
#define FRICTION 0.998f
#define BALL_RADIUS 4
#define FLIPPER_LENGTH 42
#define FLIPPER_WIDTH 7
#define FLIPPER_SPEED 0.6f
#define BALL_SPEED_MAX 7.0f
#define FLIPPER_HIT_POWER 6.5f
#define PHYSICS_SUBSTEPS 5

// === STRUCTURES ===

struct Ball {
  float x, y;
  float vx, vy;
  bool active;
  bool isMine;       // Est-ce que JE contrôle cette balle?
  uint8_t ownerID;   // ID du propriétaire original
  uint16_t color;    // Couleur de la balle (basée sur ownerID)
  float trail[6][2];
  int trailIdx;
};

struct Flipper {
  float baseX, baseY;
  float angle, targetAngle;
  float angleDown, angleUp;
  bool isLeft;
};

struct Bumper {
  float x, y;
  float radius;
  int points;
  uint32_t lastHit;
  bool lit;
  uint16_t color;
  uint16_t glowColor;
};

struct Target {
  float x, y;
  float w, h;
  int points;
  bool hit;
  uint32_t hitTime;
};

struct Slingshot {
  float x1, y1, x2, y2, x3, y3;
  bool isLeft;
  uint32_t lastHit;
  bool lit;
};

// === STRUCTURE MESSAGE ESP-NOW ===
typedef struct {
  uint8_t type;           // 0=ping, 1=ball_send, 2=ball_lost, 3=ball_position, 4=ball_score
  uint8_t senderID;       // ID unique
  uint8_t originalOwnerID; // ID du propriétaire original de la balle
  uint8_t canReceive;     // 1 si ce joueur peut recevoir des balles
  uint16_t ownerColor;    // Couleur du propriétaire de la balle
  float ballX;
  float ballVx;
  float ballVy;
  int32_t score;
  uint8_t lives;
  uint8_t wasOwnedByMe;
} GameMessage;

// === STRUCTURE PEER ===
#define MAX_PEERS 10
struct PeerInfo {
  uint8_t mac[6];
  uint8_t id;
  uint32_t lastSeen;
  bool active;
  bool canReceive;  // Ce peer peut-il recevoir des balles?
};

// === VARIABLES GLOBALES ===

Ball myBall;
Ball enemyBall;
Ball multiBalls[2];

Flipper leftFlipper, rightFlipper;

#define NUM_BUMPERS 3
#define NUM_TARGETS 5
#define NUM_SLINGSHOTS 2
Bumper bumpers[NUM_BUMPERS];
Target targets[NUM_TARGETS];
Slingshot slingshots[NUM_SLINGSHOTS];

int32_t score = 0;
int ballsLeft = 3;
bool gameOver = false;
bool gameStarted = false;  // True dès qu'on lance une balle
uint32_t frameCount = 0;

bool myBallAtEnemy = false;
float myBallEnemyPosX = 0;  // Position X de ma balle chez l'ennemi (0-135)
uint8_t myBallAtPeerID = 0; // ID du peer qui a ma balle
uint32_t lastBallPosReceived = 0;  // Dernier update de position reçu
uint32_t myBallSentTime = 0;  // Quand ma balle a été envoyée chez l'ennemi

// Timeout: si pas de nouvelles de la balle depuis X ms, considérer qu'elle est perdue
#define BALL_AT_ENEMY_TIMEOUT 2000  // 2 secondes

// Pour traquer le propriétaire de la balle ennemie qu'on a
uint8_t enemyBallOwnerID = 0;  // ID du joueur à qui appartient la balle ennemie chez nous

// Système multiball basé sur les points de la balle courante
int32_t currentBallScore = 0;           // Points accumulés par la balle courante (une seule balle en jeu)
bool multiballActive = false;

// Seuils multiball
#define MULTIBALL_THRESHOLD_MULTI 2000  // Mode multi: 2000 points
#define MULTIBALL_THRESHOLD_SOLO  3000  // Mode solo: 3000 points

bool leftButtonPressed = false;
bool rightButtonPressed = false;
bool leftWasPressed = false;
bool rightWasPressed = false;
uint32_t leftPressTime = 0;
uint32_t rightPressTime = 0;
bool bothButtonsPressed = false;
uint32_t lastBothPressed = 0;
#define FLIP_ACTIVE_TIME 180

// === MULTI-JOUEURS ===
PeerInfo peers[MAX_PEERS];
int numPeers = 0;
uint8_t myID = 0;
uint16_t myColor = 0x07FF;  // Ma couleur (sera recalculée dynamiquement)
uint8_t myColorIndex = 0;   // Mon index de couleur (0-9)
uint8_t myMac[6];

// Obtenir la couleur d'un joueur à partir de son index
uint16_t getPlayerColor(uint8_t index) {
  return PLAYER_COLORS[index % 10];
}

// Recalculer ma couleur basée sur mon rang parmi tous les joueurs
void updateMyColor() {
  // Collecter tous les IDs (moi + peers actifs)
  uint8_t allIDs[MAX_PEERS + 1];
  int count = 0;
  
  allIDs[count++] = myID;
  
  for (int i = 0; i < MAX_PEERS; i++) {
    if (peers[i].active) {
      allIDs[count++] = peers[i].id;
    }
  }
  
  // Trier les IDs
  for (int i = 0; i < count - 1; i++) {
    for (int j = i + 1; j < count; j++) {
      if (allIDs[j] < allIDs[i]) {
        uint8_t temp = allIDs[i];
        allIDs[i] = allIDs[j];
        allIDs[j] = temp;
      }
    }
  }
  
  // Trouver mon rang dans la liste triée
  uint8_t newColorIndex = 0;
  for (int i = 0; i < count; i++) {
    if (allIDs[i] == myID) {
      newColorIndex = i;
      break;
    }
  }
  
  // Mettre à jour si changement
  if (newColorIndex != myColorIndex || myBall.color != getPlayerColor(newColorIndex)) {
    myColorIndex = newColorIndex;
    myColor = getPlayerColor(myColorIndex);
    
    Serial.print("Color update: rank ");
    Serial.print(myColorIndex);
    Serial.print("/");
    Serial.print(count);
    Serial.print(" -> color 0x");
    Serial.println(myColor, HEX);
    
    // Mettre à jour la couleur de mes balles actives
    myBall.color = myColor;
    myBall.ownerID = myID;
    for (int i = 0; i < 2; i++) {
      if (multiBalls[i].isMine) {
        multiBalls[i].color = myColor;
        multiBalls[i].ownerID = myID;
      }
    }
  }
}

uint32_t lastPingSent = 0;
#define PING_INTERVAL 500
#define PEER_TIMEOUT 3000

#define WIFI_CHANNEL 1

// Prototypes
void initGame();
void initBall(Ball &ball, bool isMine, bool resetScore = true);
void checkBallAtEnemyTimeout();  // Vérifie si la balle chez l'ennemi a timeout
bool canLaunchBall();
int countMyActiveBalls();
void triggerMultiball();
void checkMultiballTrigger();  // Vérifier si on doit déclencher un multiball
void updatePhysics(Ball &ball);
void updateFlippers();
void checkCollisions(Ball &ball);
void checkBallTransfer(Ball &ball);
void drawGame();
void drawBackground();
void drawFlipper(Flipper &f);
void drawBumper(Bumper &b);
void drawTarget(Target &t);
void drawSlingshot(Slingshot &s);
void drawScoreBar();
void drawBall(Ball &ball);
void drawConnectionStatus();
bool lineCircleCollision(float x1, float y1, float x2, float y2, float cx, float cy, float r, float &nx, float &ny, float &pen);
void reflectBall(Ball &ball, float nx, float ny, float bounce);

void initESPNow();
void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status);
void onDataRecv(const uint8_t *mac, const uint8_t *data, int len);
void sendPing();
void sendBallToRandomPeer(Ball &ball);
void sendBallToOwner(Ball &ball);  // Renvoyer une balle ennemie à son propriétaire
void sendBallLost();
void sendBallLostForBall(Ball &ball);  // Notifier le propriétaire qu'une balle est tombée
void sendBallPosition(Ball &ball);  // Envoyer la position de la balle ennemie à son propriétaire
void sendScoreToOwner(int points);  // Envoyer des points au propriétaire de la balle ennemie
int findPeer(const uint8_t *mac);
int findPeerByID(uint8_t id);
int addPeer(const uint8_t *mac);
void updatePeers();
int getActivePeerCount();
int getReceivablePeerCount();  // Peers qui peuvent recevoir des balles
PeerInfo* getRandomReceivablePeer();  // Choisir parmi ceux qui peuvent recevoir

// ============================================
// SETUP
// ============================================

#if ENABLE_DEEP_SLEEP
// Fonction pour entrer en deep sleep (économie de batterie)
void enterDeepSleep() {
  Serial.println("Entering deep sleep...");
  
  // Éteindre l'écran
  tft.fillScreen(TFT_BLACK);
  digitalWrite(TFT_BL, LOW);  // Éteindre le backlight (GPIO 4 sur TTGO T-Display)
  
  // Désactiver le WiFi
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  
  // Configurer le réveil sur GPIO 13 quand il passe à HIGH
  esp_sleep_enable_ext0_wakeup((gpio_num_t)POWER_SWITCH_PIN, HIGH);
  
  // Entrer en deep sleep
  esp_deep_sleep_start();
}
#endif

void setup() {
  Serial.begin(115200);
  Serial.println("NEON PINBALL MULTIPLAYER");
  
  // Deep sleep activé ?
  #if ENABLE_DEEP_SLEEP
    // Configurer l'interrupteur ON/OFF
    pinMode(POWER_SWITCH_PIN, INPUT_PULLDOWN);
    
    // Vérifier si l'interrupteur est en position OFF
    if (digitalRead(POWER_SWITCH_PIN) == LOW) {
      // Configurer le réveil avant d'entrer en deep sleep
      esp_sleep_enable_ext0_wakeup((gpio_num_t)POWER_SWITCH_PIN, HIGH);
      Serial.println("Switch OFF - entering deep sleep");
      esp_deep_sleep_start();
    }
  #endif
  
  pinMode(BTN_LEFT, INPUT_PULLUP);
  pinMode(BTN_RIGHT, INPUT_PULLUP);
  
  // Allumer le backlight
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  
  tft.init();
  tft.setRotation(0);
  tft.fillScreen(COL_BG_DARK);
  
  buffer.createSprite(SCREEN_W, SCREEN_H);
  buffer.setTextSize(1);
  
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(1);
  tft.setCursor(10, 100);
  tft.print("Starting...");
  
  initESPNow();
  initGame();
  
  Serial.print("My ID: ");
  Serial.println(myID);
}

// ============================================
// ESP-NOW MULTI-JOUEURS
// ============================================

void initESPNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  
  WiFi.macAddress(myMac);
  // Utiliser XOR des 3 derniers octets pour un ID plus unique
  myID = myMac[3] ^ myMac[4] ^ myMac[5];
  myColorIndex = 0;
  myColor = getPlayerColor(0);  // Couleur par défaut, sera mise à jour
  
  Serial.print("My MAC: ");
  for (int i = 0; i < 6; i++) {
    Serial.printf("%02X", myMac[i]);
    if (i < 5) Serial.print(":");
  }
  Serial.print(" -> ID: ");
  Serial.println(myID);
  
  for (int i = 0; i < MAX_PEERS; i++) {
    peers[i].active = false;
  }
  numPeers = 0;
  
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed!");
    return;
  }
  
  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);
  
  esp_now_peer_info_t broadcastPeer = {};
  memset(broadcastPeer.peer_addr, 0xFF, 6);
  broadcastPeer.channel = WIFI_CHANNEL;
  broadcastPeer.encrypt = false;
  esp_now_add_peer(&broadcastPeer);
  
  Serial.println("ESP-NOW ready (multiplayer)");
}

void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
}

void onDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
  if (len != sizeof(GameMessage)) return;
  
  GameMessage msg;
  memcpy(&msg, data, sizeof(GameMessage));
  
  if (msg.senderID == myID) return;
  
  int peerIdx = findPeer(mac);
  if (peerIdx < 0) {
    peerIdx = addPeer(mac);
    if (peerIdx >= 0) {
      Serial.print("Player joined! ID:");
      Serial.print(peers[peerIdx].id);
      Serial.print(" Total:");
      Serial.println(getActivePeerCount() + 1);
    }
  }
  
  if (peerIdx >= 0) {
    peers[peerIdx].lastSeen = millis();
    peers[peerIdx].active = true;
    peers[peerIdx].canReceive = (msg.canReceive == 1);
  }
  
  switch (msg.type) {
    case 0: // PING
      // Info déjà mise à jour ci-dessus
      break;
      
    case 1: // BALL_SEND
      // Si c'est MA balle qui revient, toujours l'accepter
      if (msg.originalOwnerID == myID) {
        // C'est MA balle qui revient!
        myBall.active = true;
        myBall.isMine = true;
        myBall.ownerID = myID;
        myBall.color = myColor;
        myBall.x = SCREEN_W - msg.ballX;
        myBall.y = GAME_TOP + BALL_RADIUS + 5;
        myBall.vx = -msg.ballVx;
        myBall.vy = fabs(msg.ballVy) * 0.8f + 1.0f;
        myBall.trailIdx = 0;
        for (int i = 0; i < 6; i++) {
          myBall.trail[i][0] = myBall.x;
          myBall.trail[i][1] = myBall.y;
        }
        myBallAtEnemy = false;
        Serial.println(">>> My ball returned! <<<");
        break;
      }
      
      // C'est une balle ennemie - vérifier si on peut recevoir
      if (!gameStarted || gameOver) {
        Serial.println("Can't receive enemy ball - not ready");
        break;
      }
      
      {
        // Balle ennemie
        enemyBall.active = true;
        enemyBall.isMine = false;
        enemyBall.ownerID = msg.originalOwnerID;
        enemyBall.color = msg.ownerColor;
        enemyBallOwnerID = msg.originalOwnerID;
        enemyBall.x = SCREEN_W - msg.ballX;
        enemyBall.y = GAME_TOP + BALL_RADIUS + 5;
        enemyBall.vx = -msg.ballVx;
        enemyBall.vy = fabs(msg.ballVy) * 0.8f + 1.0f;
        enemyBall.trailIdx = 0;
        for (int i = 0; i < 6; i++) {
          enemyBall.trail[i][0] = enemyBall.x;
          enemyBall.trail[i][1] = enemyBall.y;
        }
        Serial.print("Enemy ball from player ");
        Serial.print(msg.senderID);
        Serial.print(" (owner: ");
        Serial.print(msg.originalOwnerID);
        Serial.println(")");
      }
      break;
      
    case 2: // BALL_LOST - une balle est tombée chez l'ennemi
      if (msg.originalOwnerID == myID) {
        // Ma balle est tombée chez un ennemi - PAS de perte de vie
        // Je peux simplement relancer une nouvelle balle
        myBallAtEnemy = false;
        Serial.println("My ball fell at enemy - can relaunch (no life lost)");
      }
      break;
      
    case 3: // BALL_POSITION - update de position de ma balle chez l'ennemi
      if (msg.originalOwnerID == myID && myBallAtEnemy) {
        myBallEnemyPosX = msg.ballX;
        lastBallPosReceived = millis();
      }
      break;
      
    case 4: // BALL_SCORE - ma balle a marqué des points chez l'ennemi
      if (msg.originalOwnerID == myID) {
        int points = (int)msg.ballX;
        score += points;
        // Ajouter au compteur seulement si une seule balle en jeu (pas pendant multiball)
        if (myBallAtEnemy && countMyActiveBalls() <= 1) {
          currentBallScore += points;
        }
        Serial.print("Points from enemy: +");
        Serial.println(points);
      }
      break;
  }
}

int findPeer(const uint8_t *mac) {
  for (int i = 0; i < MAX_PEERS; i++) {
    if (peers[i].active && memcmp(peers[i].mac, mac, 6) == 0) {
      return i;
    }
  }
  return -1;
}

int addPeer(const uint8_t *mac) {
  int slot = -1;
  for (int i = 0; i < MAX_PEERS; i++) {
    if (!peers[i].active) {
      slot = i;
      break;
    }
  }
  
  if (slot < 0) return -1;
  
  memcpy(peers[slot].mac, mac, 6);
  // Utiliser XOR des 3 derniers octets pour un ID plus unique
  peers[slot].id = mac[3] ^ mac[4] ^ mac[5];
  peers[slot].lastSeen = millis();
  peers[slot].active = true;
  
  if (!esp_now_is_peer_exist(mac)) {
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, mac, 6);
    peerInfo.channel = WIFI_CHANNEL;
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);
  }
  
  numPeers++;
  
  // Recalculer les couleurs
  updateMyColor();
  
  Serial.print("Peer added: ID ");
  Serial.print(peers[slot].id);
  Serial.print(", total peers: ");
  Serial.println(numPeers);
  
  return slot;
}

void updatePeers() {
  uint32_t now = millis();
  bool changed = false;
  for (int i = 0; i < MAX_PEERS; i++) {
    if (peers[i].active && now - peers[i].lastSeen > PEER_TIMEOUT) {
      Serial.print("Player ");
      Serial.print(peers[i].id);
      Serial.println(" left");
      peers[i].active = false;
      numPeers--;
      changed = true;
    }
  }
  
  // Recalculer les couleurs si un joueur est parti
  if (changed) {
    updateMyColor();
  }
}

int getActivePeerCount() {
  int count = 0;
  for (int i = 0; i < MAX_PEERS; i++) {
    if (peers[i].active) count++;
  }
  return count;
}

void sendPing() {
  GameMessage msg;
  msg.type = 0;
  msg.senderID = myID;
  msg.originalOwnerID = 0;
  msg.ownerColor = myColor;
  msg.canReceive = (gameStarted && !gameOver) ? 1 : 0;
  msg.score = score;
  msg.lives = ballsLeft;
  msg.ballX = 0;
  msg.ballVx = 0;
  msg.ballVy = 0;
  msg.wasOwnedByMe = 0;
  
  uint8_t broadcast[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_send(broadcast, (uint8_t*)&msg, sizeof(GameMessage));
}

int findPeerByID(uint8_t id) {
  for (int i = 0; i < MAX_PEERS; i++) {
    if (peers[i].active && peers[i].id == id) {
      return i;
    }
  }
  return -1;
}

int getReceivablePeerCount() {
  int count = 0;
  for (int i = 0; i < MAX_PEERS; i++) {
    if (peers[i].active && peers[i].canReceive) count++;
  }
  return count;
}

PeerInfo* getRandomReceivablePeer() {
  int receivableCount = getReceivablePeerCount();
  if (receivableCount == 0) return NULL;
  
  int target = random(receivableCount);
  int current = 0;
  
  for (int i = 0; i < MAX_PEERS; i++) {
    if (peers[i].active && peers[i].canReceive) {
      if (current == target) return &peers[i];
      current++;
    }
  }
  return NULL;
}

void sendBallToRandomPeer(Ball &ball) {
  PeerInfo* target = getRandomReceivablePeer();
  if (!target) {
    // Personne ne peut recevoir - la balle rebondit
    ball.y = GAME_TOP + BALL_RADIUS;
    ball.vy = fabs(ball.vy) * 0.8f;
    Serial.println("No peer can receive - ball bounces");
    return;
  }
  
  GameMessage msg;
  msg.type = 1;
  msg.senderID = myID;
  // Garder l'owner original et sa couleur
  msg.originalOwnerID = ball.ownerID;
  msg.ownerColor = ball.color;
  msg.canReceive = (gameStarted && !gameOver) ? 1 : 0;
  msg.ballX = ball.x;
  msg.ballVx = ball.vx;
  msg.ballVy = ball.vy;
  msg.score = score;
  msg.lives = ballsLeft;
  msg.wasOwnedByMe = ball.isMine ? 1 : 0;
  
  esp_now_send(target->mac, (uint8_t*)&msg, sizeof(GameMessage));
  
  // Noter où est partie ma balle
  if (ball.isMine) {
    myBallAtPeerID = target->id;
  }
  
  Serial.print("Ball -> player ");
  Serial.println(target->id);
  
  ball.active = false;
}

// Notifier le propriétaire qu'une balle ennemie est tombée
void sendBallLostForBall(Ball &ball) {
  GameMessage msg;
  msg.type = 2;
  msg.senderID = myID;
  msg.originalOwnerID = ball.ownerID;  // Le propriétaire de la balle qui est tombée
  msg.ownerColor = ball.color;
  msg.canReceive = (gameStarted && !gameOver) ? 1 : 0;
  msg.score = score;
  msg.lives = ballsLeft;
  msg.ballX = 0;
  msg.ballVx = 0;
  msg.ballVy = 0;
  msg.wasOwnedByMe = 0;
  
  // Broadcast à tous (le propriétaire original va récupérer)
  uint8_t broadcast[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_send(broadcast, (uint8_t*)&msg, sizeof(GameMessage));
  
  Serial.print("Ball lost notification sent to owner ");
  Serial.println(ball.ownerID);
}

// Ancienne fonction pour compatibilité (utilise enemyBall)
void sendBallLost() {
  sendBallLostForBall(enemyBall);
}

// Renvoyer une balle ennemie à son propriétaire (quand elle sort par le haut)
void sendBallToOwner(Ball &ball) {
  GameMessage msg;
  msg.type = 1;  // BALL_SEND
  msg.senderID = myID;
  msg.originalOwnerID = ball.ownerID;
  msg.ownerColor = ball.color;
  msg.canReceive = (gameStarted && !gameOver) ? 1 : 0;
  msg.ballX = ball.x;
  msg.ballVx = ball.vx;
  msg.ballVy = ball.vy;
  msg.score = score;
  msg.lives = ballsLeft;
  msg.wasOwnedByMe = 0;
  
  // Broadcast - le propriétaire va recevoir sa balle
  uint8_t broadcast[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_send(broadcast, (uint8_t*)&msg, sizeof(GameMessage));
  
  Serial.print("Ball returned to owner ");
  Serial.println(ball.ownerID);
  
  ball.active = false;
}

// Envoyer des points au propriétaire de la balle ennemie
void sendScoreToOwner(int points) {
  if (enemyBallOwnerID == 0) return;
  
  GameMessage msg;
  msg.type = 4;  // BALL_SCORE
  msg.senderID = myID;
  msg.originalOwnerID = enemyBallOwnerID;
  msg.ownerColor = enemyBall.color;
  msg.canReceive = (gameStarted && !gameOver) ? 1 : 0;
  msg.ballX = points;  // Utiliser ballX pour les points
  msg.ballVx = 0;
  msg.ballVy = 0;
  msg.score = score;
  msg.lives = ballsLeft;
  msg.wasOwnedByMe = 0;
  
  // Broadcast (le propriétaire va filtrer par originalOwnerID)
  uint8_t broadcast[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_send(broadcast, (uint8_t*)&msg, sizeof(GameMessage));
}

// Envoyer la position de la balle ennemie à son propriétaire
void sendBallPosition(Ball &ball) {
  if (ball.isMine || !ball.active) return;
  
  // Trouver le peer propriétaire et lui envoyer directement
  int peerIdx = findPeerByID(enemyBallOwnerID);
  if (peerIdx < 0) {
    // Broadcast si on ne trouve pas le propriétaire
    GameMessage msg;
    msg.type = 3;
    msg.senderID = myID;
    msg.originalOwnerID = enemyBallOwnerID;
    msg.ownerColor = ball.color;
    msg.canReceive = (gameStarted && !gameOver) ? 1 : 0;
    msg.ballX = ball.x;
    msg.ballVx = 0;
    msg.ballVy = 0;
    msg.score = score;
    msg.lives = ballsLeft;
    msg.wasOwnedByMe = 0;
    
    uint8_t broadcast[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    esp_now_send(broadcast, (uint8_t*)&msg, sizeof(GameMessage));
  } else {
    GameMessage msg;
    msg.type = 3;
    msg.senderID = myID;
    msg.originalOwnerID = enemyBallOwnerID;
    msg.ownerColor = ball.color;
    msg.canReceive = (gameStarted && !gameOver) ? 1 : 0;
    msg.ballX = ball.x;
    msg.ballVx = 0;
    msg.ballVy = 0;
    msg.score = score;
    msg.lives = ballsLeft;
    msg.wasOwnedByMe = 0;
    
    esp_now_send(peers[peerIdx].mac, (uint8_t*)&msg, sizeof(GameMessage));
  }
}

// ============================================
// GAME FUNCTIONS
// ============================================

void initGame() {
  leftFlipper.baseX = 15;
  leftFlipper.baseY = SCREEN_H - 28;
  leftFlipper.angleDown = 0.35f;
  leftFlipper.angleUp = -0.65f;
  leftFlipper.angle = leftFlipper.angleDown;
  leftFlipper.targetAngle = leftFlipper.angleDown;
  leftFlipper.isLeft = true;
  
  rightFlipper.baseX = SCREEN_W - 15;
  rightFlipper.baseY = SCREEN_H - 28;
  rightFlipper.angleDown = PI - 0.35f;
  rightFlipper.angleUp = PI + 0.65f;
  rightFlipper.angle = rightFlipper.angleDown;
  rightFlipper.targetAngle = rightFlipper.angleDown;
  rightFlipper.isLeft = false;
  
  bumpers[0] = {SCREEN_W / 2.0f, GAME_TOP + 50, 11, 100, 0, false, COL_BUMPER_1, COL_NEON_PINK};
  bumpers[1] = {25, GAME_TOP + 80, 9, 50, 0, false, COL_BUMPER_2, COL_GOLD};
  bumpers[2] = {SCREEN_W - 25, GAME_TOP + 80, 9, 50, 0, false, COL_BUMPER_3, 0x001F};
  
  float targetY = GAME_TOP + 115;
  float targetSpacing = 22;
  float startX = (SCREEN_W - 4 * targetSpacing) / 2;
  for (int i = 0; i < 5; i++) {
    targets[i] = {startX + i * targetSpacing, targetY, 8, 14, 50, false, 0};
  }
  
  slingshots[0] = {5, SCREEN_H - 95, 5, SCREEN_H - 55, 25, SCREEN_H - 55, true, 0, false};
  slingshots[1] = {SCREEN_W - 5, SCREEN_H - 95, SCREEN_W - 5, SCREEN_H - 55, SCREEN_W - 25, SCREEN_H - 55, false, 0, false};
  
  myBall.active = false;
  myBall.isMine = true;
  myBall.ownerID = myID;
  myBall.color = myColor;
  myBall.trailIdx = 0;
  
  enemyBall.active = false;
  enemyBall.isMine = false;
  enemyBall.ownerID = 0;
  enemyBall.color = 0xFFFF;
  enemyBall.trailIdx = 0;
  
  for (int i = 0; i < 2; i++) {
    multiBalls[i].active = false;
    multiBalls[i].isMine = true;
    multiBalls[i].ownerID = myID;
    multiBalls[i].color = myColor;
    multiBalls[i].trailIdx = 0;
  }
  
  myBallAtEnemy = false;
  myBallEnemyPosX = 0;
  myBallAtPeerID = 0;
  lastBallPosReceived = 0;
  myBallSentTime = 0;
  currentBallScore = 0;
  multiballActive = false;
  
  ballsLeft = 3;
  score = 0;
  gameOver = false;
  gameStarted = false;
}

void initBall(Ball &ball, bool isMine, bool resetScore) {
  ball.x = SCREEN_W / 2 + (random(40) - 20);
  ball.y = GAME_TOP + 30 + random(10);
  
  float angle = (random(100) / 100.0f) * PI - PI/2;
  float speed = 1.5f + (random(100) / 100.0f) * 1.0f;
  ball.vx = cos(angle) * speed * 0.5f;
  ball.vy = fabs(sin(angle)) * speed * 0.3f + 0.5f;
  
  ball.active = true;
  ball.isMine = isMine;
  ball.ownerID = myID;
  ball.color = myColor;
  ball.trailIdx = 0;
  for (int i = 0; i < 6; i++) {
    ball.trail[i][0] = ball.x;
    ball.trail[i][1] = ball.y;
  }
  
  // Marquer que la partie a commencé
  if (isMine) {
    gameStarted = true;
    
    // Reset le compteur seulement si demandé (nouvelle session de balle)
    if (resetScore) {
      currentBallScore = 0;
    }
  }
}

// Vérifie si la balle chez l'ennemi a timeout (pas de nouvelles depuis trop longtemps)
void checkBallAtEnemyTimeout() {
  if (!myBallAtEnemy) return;
  
  uint32_t now = millis();
  uint32_t lastUpdate = max(lastBallPosReceived, myBallSentTime);
  
  if (now - lastUpdate > BALL_AT_ENEMY_TIMEOUT) {
    Serial.println("!!! Ball at enemy TIMEOUT - assuming lost !!!");
    Serial.print("    Last update: ");
    Serial.print(now - lastUpdate);
    Serial.println(" ms ago");
    myBallAtEnemy = false;
    // La balle était chez l'ennemi, donc PAS de perte de vie
    // Le joueur peut simplement relancer
    Serial.println("Can relaunch (no life lost)");
  }
}

bool canLaunchBall() {
  if (myBall.active) return false;
  
  // Vérifier le timeout de la balle chez l'ennemi
  if (myBallAtEnemy) {
    checkBallAtEnemyTimeout();
    if (myBallAtEnemy) return false;  // Toujours chez l'ennemi après vérification
  }
  
  // Vérifier si des multiballs sont encore actives
  for (int i = 0; i < 2; i++) {
    if (multiBalls[i].active) return false;
  }
  return true;
}

// Compte le nombre de mes balles actives (myBall + multiballs)
int countMyActiveBalls() {
  int count = 0;
  if (myBall.active) count++;
  for (int i = 0; i < 2; i++) {
    if (multiBalls[i].active) count++;
  }
  return count;
}

void triggerMultiball() {
  Serial.println("=== MULTIBALL! ===");
  multiballActive = true;
  
  // Les deux balles sortent du bumper central (SCREEN_W/2, GAME_TOP+50)
  // Direction: vers les côtés et légèrement vers le haut
  for (int i = 0; i < 2; i++) {
    // Position: juste à côté du bumper central
    float offsetX = (i == 0) ? -15 : 15;  // Gauche ou droite du bumper
    multiBalls[i].x = SCREEN_W / 2.0f + offsetX;
    multiBalls[i].y = GAME_TOP + 50;  // Même hauteur que le bumper central
    
    // Direction: vers le côté et légèrement vers le haut
    float dirX = (i == 0) ? -1.0f : 1.0f;  // Gauche ou droite
    float dirY = -0.3f;  // Légèrement vers le haut
    
    float speed = 3.5f + (random(100) / 100.0f);
    multiBalls[i].vx = dirX * speed;
    multiBalls[i].vy = dirY * speed;
    
    multiBalls[i].active = true;
    multiBalls[i].isMine = true;
    multiBalls[i].ownerID = myID;
    multiBalls[i].color = myColor;
    multiBalls[i].trailIdx = 0;
    for (int j = 0; j < 6; j++) {
      multiBalls[i].trail[j][0] = multiBalls[i].x;
      multiBalls[i].trail[j][1] = multiBalls[i].y;
    }
  }
  
  // Pendant le multiball, on ne compte plus les points
  // Le compteur sera reset quand il ne restera qu'une balle
  Serial.println("Multiball active - score counting paused");
}

// Vérifier si on doit déclencher un multiball
void checkMultiballTrigger() {
  // Ne pas déclencher pendant un multiball actif
  if (multiballActive) return;
  
  int threshold = (getActivePeerCount() > 0) ? MULTIBALL_THRESHOLD_MULTI : MULTIBALL_THRESHOLD_SOLO;
  if (currentBallScore >= threshold) {
    triggerMultiball();
  }
}

void updatePhysics(Ball &ball) {
  if (!ball.active) return;
  
  float subGravity = GRAVITY / PHYSICS_SUBSTEPS;
  ball.vy += subGravity;
  ball.vx *= FRICTION;
  ball.vy *= FRICTION;
  
  float speed = sqrt(ball.vx * ball.vx + ball.vy * ball.vy);
  if (speed > BALL_SPEED_MAX) {
    ball.vx = (ball.vx / speed) * BALL_SPEED_MAX;
    ball.vy = (ball.vy / speed) * BALL_SPEED_MAX;
  }
  
  ball.x += ball.vx / PHYSICS_SUBSTEPS;
  ball.y += ball.vy / PHYSICS_SUBSTEPS;
}

void updateFlippers() {
  float diffL = leftFlipper.targetAngle - leftFlipper.angle;
  if (fabs(diffL) < 0.02f) {
    leftFlipper.angle = leftFlipper.targetAngle;
  } else {
    float moveL = diffL * FLIPPER_SPEED;
    if (fabs(moveL) < 0.05f) moveL = (diffL > 0) ? 0.05f : -0.05f;
    leftFlipper.angle += moveL;
  }
  
  float diffR = rightFlipper.targetAngle - rightFlipper.angle;
  if (fabs(diffR) < 0.02f) {
    rightFlipper.angle = rightFlipper.targetAngle;
  } else {
    float moveR = diffR * FLIPPER_SPEED;
    if (fabs(moveR) < 0.05f) moveR = (diffR > 0) ? 0.05f : -0.05f;
    rightFlipper.angle += moveR;
  }
}

void checkBallTransfer(Ball &ball) {
  if (!ball.active) return;
  
  bool isMultiball = false;
  for (int i = 0; i < 2; i++) {
    if (&ball == &multiBalls[i]) isMultiball = true;
  }
  
  int receivableCount = getReceivablePeerCount();
  
  // Sort par le haut
  if (ball.y < GAME_TOP - BALL_RADIUS) {
    if (ball.isMine) {
      // C'est MA balle - l'envoyer à un joueur aléatoire
      if (receivableCount == 0) {
        // Personne ne peut recevoir: rebondir
        ball.y = GAME_TOP + BALL_RADIUS;
        ball.vy = fabs(ball.vy) * 0.8f;
      } else {
        if (&ball == &myBall) {
          myBallAtEnemy = true;
          myBallEnemyPosX = ball.x;
          myBallSentTime = millis();  // Noter le moment de l'envoi pour le timeout
          Serial.println(">>> My ball sent to enemy <<<");
        }
        sendBallToRandomPeer(ball);
      }
    } else {
      // C'est une balle ENNEMIE - la renvoyer à son propriétaire
      sendBallToOwner(ball);
    }
    return;
  }
  
  // Tombe en bas
  if (ball.y > SCREEN_H + BALL_RADIUS * 2) {
    if (ball.isMine) {
      ball.active = false;
      
      // Compter les balles restantes APRÈS cette chute
      int remainingBalls = countMyActiveBalls();
      
      Serial.print("Ball fell! Remaining balls: ");
      Serial.println(remainingBalls);
      
      // On perd une vie SEULEMENT si plus aucune balle en jeu
      if (remainingBalls == 0) {
        ballsLeft--;
        Serial.print("No more balls in play - lost a life! Lives: ");
        Serial.println(ballsLeft);
        if (ballsLeft <= 0) {
          gameOver = true;
          Serial.println("GAME OVER!");
        }
      }
      
      // Reset multiball si terminé (1 balle ou moins)
      if (remainingBalls <= 1) {
        currentBallScore = 0;
        if (multiballActive) {
          multiballActive = false;
          Serial.println("Multiball ended");
        }
      }
      
    } else {
      // C'est une balle ENNEMIE qui tombe - notifier le propriétaire
      ball.active = false;
      sendBallLostForBall(ball);
    }
  }
}

void checkCollisions(Ball &ball) {
  if (!ball.active) return;
  
  float nx, ny, penetration;
  uint32_t currentTime = millis();
  
  // Déterminer si c'est une de mes balles (pour le tracking multiball)
  bool isMyBallOrMultiball = ball.isMine;
  int pointMultiplier = ball.isMine ? 1 : -1;
  
  // On compte les points pour le multiball SEULEMENT si une seule balle en jeu
  bool countForMultiball = ball.isMine && (countMyActiveBalls() == 1);
  
  // Murs
  if (ball.x - BALL_RADIUS < 0) {
    ball.x = BALL_RADIUS;
    ball.vx = -ball.vx * 0.75f;
  }
  if (ball.x + BALL_RADIUS > SCREEN_W) {
    ball.x = SCREEN_W - BALL_RADIUS;
    ball.vx = -ball.vx * 0.75f;
  }
  
  // Guides
  float wallLeftX = 15;
  float wallRightX = SCREEN_W - 15;
  float wallTop = SCREEN_H - 80;
  float wallBottom = SCREEN_H - 35;
  
  if (ball.x - BALL_RADIUS < wallLeftX && ball.y > wallTop && ball.y < wallBottom) {
    ball.x = wallLeftX + BALL_RADIUS;
    ball.vx = fabs(ball.vx) * 0.7f;
  }
  if (ball.x + BALL_RADIUS > wallRightX && ball.y > wallTop && ball.y < wallBottom) {
    ball.x = wallRightX - BALL_RADIUS;
    ball.vx = -fabs(ball.vx) * 0.7f;
  }
  
  // Rampes
  if (lineCircleCollision(wallLeftX, wallBottom, leftFlipper.baseX, leftFlipper.baseY + 3, ball.x, ball.y, BALL_RADIUS, nx, ny, penetration)) {
    ball.x += nx * penetration;
    ball.y += ny * penetration;
    reflectBall(ball, nx, ny, 0.7f);
  }
  if (lineCircleCollision(wallRightX, wallBottom, rightFlipper.baseX, rightFlipper.baseY + 3, ball.x, ball.y, BALL_RADIUS, nx, ny, penetration)) {
    ball.x += nx * penetration;
    ball.y += ny * penetration;
    reflectBall(ball, nx, ny, 0.7f);
  }
  
  // Flippers
  Flipper* flippers[2] = {&leftFlipper, &rightFlipper};
  bool* btnPressed[2] = {&leftButtonPressed, &rightButtonPressed};
  uint32_t* pressTime[2] = {&leftPressTime, &rightPressTime};
  
  for (int f = 0; f < 2; f++) {
    Flipper* flip = flippers[f];
    float baseX = flip->baseX;
    float baseY = flip->baseY;
    float angle = flip->angle;
    float endX = baseX + cos(angle) * FLIPPER_LENGTH;
    float endY = baseY + sin(angle) * FLIPPER_LENGTH;
    
    // Collision avec le cercle au bout du flipper (tip)
    float tipRadius = FLIPPER_WIDTH * 0.7f + BALL_RADIUS;
    float tipDX = ball.x - endX;
    float tipDY = ball.y - endY;
    float tipDist = sqrt(tipDX * tipDX + tipDY * tipDY);
    
    if (tipDist < tipRadius && tipDist > 0.1f) {
      // Collision avec le bout du flipper
      nx = tipDX / tipDist;
      ny = tipDY / tipDist;
      
      ball.x = endX + nx * (tipRadius + 1);
      ball.y = endY + ny * (tipRadius + 1);
      
      bool isActiveHit = *btnPressed[f] && (currentTime - *pressTime[f] < FLIP_ACTIVE_TIME);
      float angularVel = (flip->targetAngle - flip->angle);
      float flipVelX = -sin(angle) * angularVel * FLIPPER_LENGTH * 0.6f;
      float flipVelY = cos(angle) * angularVel * FLIPPER_LENGTH * 0.6f;
      
      if (isActiveHit) {
        float power = FLIPPER_HIT_POWER * 1.2f;  // Plus de puissance au bout
        float tangentX = -sin(angle);
        float outX = nx * 0.4f + tangentX * 0.6f * (flip->isLeft ? 1 : -1);
        float outY = -0.9f;
        float outLen = sqrt(outX * outX + outY * outY);
        outX /= outLen;
        outY /= outLen;
        
        ball.vx = outX * power + flipVelX * 3.0f;
        ball.vy = outY * power + flipVelY * 3.0f;
        if (ball.vy > -2.0f) ball.vy = -power * 0.8f;
      } else {
        float normalVel = ball.vx * nx + ball.vy * ny;
        if (normalVel < 0) {
          ball.vx -= normalVel * nx * 0.8f;
          ball.vy -= normalVel * ny * 0.8f;
        }
      }
      continue;  // Passer au prochain flipper
    }
    
    // Collision avec le corps du flipper (ligne)
    float flipDX = endX - baseX;
    float flipDY = endY - baseY;
    float flipLen = FLIPPER_LENGTH;
    
    float toBallX = ball.x - baseX;
    float toBallY = ball.y - baseY;
    
    float t = (toBallX * flipDX + toBallY * flipDY) / (flipLen * flipLen);
    t = constrain(t, 0.0f, 1.0f);
    
    float closestX = baseX + t * flipDX;
    float closestY = baseY + t * flipDY;
    
    float distX = ball.x - closestX;
    float distY = ball.y - closestY;
    float dist = sqrt(distX * distX + distY * distY);
    
    float collisionRadius = BALL_RADIUS + FLIPPER_WIDTH * (1.0f - t * 0.3f);
    
    if (dist < collisionRadius && dist > 0.1f) {
      nx = distX / dist;
      ny = distY / dist;
      
      ball.x += nx * (collisionRadius - dist + 1.0f);
      ball.y += ny * (collisionRadius - dist + 1.0f);
      
      bool isActiveHit = *btnPressed[f] && (currentTime - *pressTime[f] < FLIP_ACTIVE_TIME);
      float angularVel = (flip->targetAngle - flip->angle);
      float flipVelX = -sin(angle) * angularVel * t * flipLen * 0.5f;
      float flipVelY = cos(angle) * angularVel * t * flipLen * 0.5f;
      
      if (isActiveHit) {
        float power = FLIPPER_HIT_POWER * (0.5f + t * 0.8f);
        float tangentX = -flipDY / flipLen;
        float outX = nx * 0.3f + tangentX * (0.5f + t * 0.5f) * (flip->isLeft ? 1 : -1);
        float outY = -0.8f - t * 0.3f;
        float outLen = sqrt(outX * outX + outY * outY);
        outX /= outLen;
        outY /= outLen;
        
        ball.vx = outX * power + flipVelX * 3.0f;
        ball.vy = outY * power + flipVelY * 3.0f;
        if (ball.vy > -1.5f) ball.vy = -power * 0.7f;
      } else if (*btnPressed[f]) {
        float relVelX = ball.vx - flipVelX;
        float relVelY = ball.vy - flipVelY;
        float normalVel = relVelX * nx + relVelY * ny;
        if (normalVel < 0) {
          ball.vx -= normalVel * nx * 0.9f;
          ball.vy -= normalVel * ny * 0.9f;
        }
      } else {
        float relVelX = ball.vx - flipVelX;
        float relVelY = ball.vy - flipVelY;
        float normalVel = relVelX * nx + relVelY * ny;
        if (normalVel < 0) {
          ball.vx -= normalVel * nx * 0.7f;
          ball.vy -= normalVel * ny * 0.7f;
        }
      }
    }
  }
  
  // Bumpers
  for (int i = 0; i < NUM_BUMPERS; i++) {
    float dx = ball.x - bumpers[i].x;
    float dy = ball.y - bumpers[i].y;
    float dist = sqrt(dx * dx + dy * dy);
    float minDist = BALL_RADIUS + bumpers[i].radius;
    
    if (dist < minDist && dist > 0) {
      nx = dx / dist;
      ny = dy / dist;
      ball.x = bumpers[i].x + nx * (minDist + 1);
      ball.y = bumpers[i].y + ny * (minDist + 1);
      
      float bounceForce = 5.0f;
      ball.vx = nx * bounceForce;
      ball.vy = ny * bounceForce;
      
      if (ny < -0.3f) {
        ball.vy -= 1.0f;
      }
      
      score += bumpers[i].points * pointMultiplier;
      if (countForMultiball) {
        currentBallScore += bumpers[i].points;
      }
      // Envoyer les points au propriétaire si c'est une balle ennemie
      if (!ball.isMine) {
        sendScoreToOwner(bumpers[i].points);
      }
      bumpers[i].lastHit = currentTime;
      bumpers[i].lit = true;
    }
  }
  
  // Targets
  for (int i = 0; i < NUM_TARGETS; i++) {
    if (!targets[i].hit) {
      if (ball.x + BALL_RADIUS > targets[i].x &&
          ball.x - BALL_RADIUS < targets[i].x + targets[i].w &&
          ball.y + BALL_RADIUS > targets[i].y &&
          ball.y - BALL_RADIUS < targets[i].y + targets[i].h) {
        ball.vy = -fabs(ball.vy) * 0.8f;
        targets[i].hit = true;
        targets[i].hitTime = currentTime;
        score += targets[i].points * pointMultiplier;
        if (countForMultiball) {
          currentBallScore += targets[i].points;
        }
        // Envoyer les points au propriétaire si c'est une balle ennemie
        if (!ball.isMine) {
          sendScoreToOwner(targets[i].points);
        }
      }
    }
  }
  
  // Slingshots
  for (int i = 0; i < NUM_SLINGSHOTS; i++) {
    Slingshot &s = slingshots[i];
    float cx = (s.x1 + s.x2 + s.x3) / 3;
    float cy = (s.y1 + s.y2 + s.y3) / 3;
    float dx = ball.x - cx;
    float dy = ball.y - cy;
    float dist = sqrt(dx * dx + dy * dy);
    
    if (dist < 20 && dist > 0) {
      if (lineCircleCollision(s.x1, s.y1, s.x3, s.y3, ball.x, ball.y, BALL_RADIUS, nx, ny, penetration) ||
          lineCircleCollision(s.x2, s.y2, s.x3, s.y3, ball.x, ball.y, BALL_RADIUS, nx, ny, penetration)) {
        ball.x += nx * (penetration + 1);
        ball.y += ny * (penetration + 1);
        
        float bounce = 4.5f;
        ball.vx = nx * bounce;
        ball.vy = ny * bounce - 1.5f;
        
        s.lit = true;
        s.lastHit = currentTime;
        score += 10 * pointMultiplier;
        if (countForMultiball) {
          currentBallScore += 10;
        }
        // Envoyer les points au propriétaire si c'est une balle ennemie
        if (!ball.isMine) {
          sendScoreToOwner(10);
        }
      }
    }
  }
}

bool lineCircleCollision(float x1, float y1, float x2, float y2, float cx, float cy, float r, float &nx, float &ny, float &pen) {
  float dx = x2 - x1, dy = y2 - y1;
  float fx = x1 - cx, fy = y1 - cy;
  float a = dx * dx + dy * dy;
  float b = 2 * (fx * dx + fy * dy);
  float c = fx * fx + fy * fy - r * r;
  float disc = b * b - 4 * a * c;
  
  if (disc < 0) {
    float d1 = sqrt(fx * fx + fy * fy);
    if (d1 < r && d1 > 0) { nx = -fx / d1; ny = -fy / d1; pen = r - d1; return true; }
    float ex = x2 - cx, ey = y2 - cy;
    float d2 = sqrt(ex * ex + ey * ey);
    if (d2 < r && d2 > 0) { nx = -ex / d2; ny = -ey / d2; pen = r - d2; return true; }
    return false;
  }
  
  disc = sqrt(disc);
  float t1 = (-b - disc) / (2 * a), t2 = (-b + disc) / (2 * a);
  float t = (t1 >= 0 && t1 <= 1) ? t1 : ((t2 >= 0 && t2 <= 1) ? t2 : -1);
  if (t < 0) return false;
  
  float closestX = x1 + t * dx, closestY = y1 + t * dy;
  float distX = cx - closestX, distY = cy - closestY;
  float dist = sqrt(distX * distX + distY * distY);
  if (dist < r && dist > 0) { nx = distX / dist; ny = distY / dist; pen = r - dist; return true; }
  return false;
}

void reflectBall(Ball &ball, float nx, float ny, float bounce) {
  float dot = ball.vx * nx + ball.vy * ny;
  ball.vx = (ball.vx - 2 * dot * nx) * bounce;
  ball.vy = (ball.vy - 2 * dot * ny) * bounce;
}

// ============================================
// MAIN LOOP
// ============================================

void loop() {
  static uint32_t lastUpdate = 0;
  uint32_t now = millis();
  
  // Vérifier l'interrupteur toutes les 100ms (si deep sleep activé)
  #if ENABLE_DEEP_SLEEP
    static uint32_t lastSwitchCheck = 0;
    if (now - lastSwitchCheck >= 100) {
      lastSwitchCheck = now;
      if (digitalRead(POWER_SWITCH_PIN) == LOW) {
        enterDeepSleep();
      }
    }
  #endif
  
  if (now - lastPingSent >= PING_INTERVAL) {
    sendPing();
    lastPingSent = now;
    updatePeers();
    
    // Envoyer périodiquement la position de la balle ennemie
    if (enemyBall.active) {
      sendBallPosition(enemyBall);
    }
  }
  
  if (now - lastUpdate >= 16) {
    lastUpdate = now;
    frameCount++;
    
    bool leftPressed = !digitalRead(BTN_LEFT);
    bool rightPressed = !digitalRead(BTN_RIGHT);
    
    if (leftPressed && !leftWasPressed) leftPressTime = now;
    if (rightPressed && !rightWasPressed) rightPressTime = now;
    
    leftButtonPressed = leftPressed;
    rightButtonPressed = rightPressed;
    leftWasPressed = leftPressed;
    rightWasPressed = rightPressed;
    
    if (leftPressed && rightPressed) {
      if (!bothButtonsPressed) {
        bothButtonsPressed = true;
        lastBothPressed = now;
      } else if (now - lastBothPressed > 200) {
        if ((canLaunchBall() || gameOver)) {
          if (gameOver) {
            score = 0;
            ballsLeft = 3;
            gameOver = false;
            gameStarted = false;  // Reset pour recommencer proprement
            currentBallScore = 0;
            multiballActive = false;
            myBallAtEnemy = false;
            myBallEnemyPosX = 0;
            for (int i = 0; i < NUM_TARGETS; i++) targets[i].hit = false;
            enemyBall.active = false;
            for (int i = 0; i < 2; i++) multiBalls[i].active = false;
          }
          if (ballsLeft > 0) {
            initBall(myBall, true);
            lastBothPressed = now + 1000;
          }
        }
      }
    } else {
      bothButtonsPressed = false;
    }
    
    // Vérifier si on doit déclencher un multiball
    if (!gameOver) {
      checkMultiballTrigger();
      
      // Vérification anti-blocage: si le joueur n'a aucune balle en jeu et peut relancer
      // mais que le bandeau ne s'affiche pas, forcer le déblocage
      if (gameStarted && !myBall.active && ballsLeft > 0) {
        bool anyMultiballActive = false;
        for (int i = 0; i < 2; i++) {
          if (multiBalls[i].active) anyMultiballActive = true;
        }
        
        if (!anyMultiballActive) {
          // Vérifier le timeout de la balle chez l'ennemi
          checkBallAtEnemyTimeout();
        }
      }
    }
    
    // Flippers: ne bougent plus en game over (sauf si balle ennemie encore en jeu)
    if (!gameOver) {
      leftFlipper.targetAngle = leftPressed ? leftFlipper.angleUp : leftFlipper.angleDown;
      rightFlipper.targetAngle = rightPressed ? rightFlipper.angleUp : rightFlipper.angleDown;
    } else {
      // Game over: flippers en position basse
      leftFlipper.targetAngle = leftFlipper.angleDown;
      rightFlipper.targetAngle = rightFlipper.angleDown;
    }
    updateFlippers();
    
    if (!gameOver) {
      if (myBall.active) {
        for (int step = 0; step < PHYSICS_SUBSTEPS; step++) {
          updatePhysics(myBall);
          checkCollisions(myBall);
        }
        checkBallTransfer(myBall);
        
        if (frameCount % 2 == 0) {
          myBall.trail[myBall.trailIdx][0] = myBall.x;
          myBall.trail[myBall.trailIdx][1] = myBall.y;
          myBall.trailIdx = (myBall.trailIdx + 1) % 6;
        }
      }
      
      for (int i = 0; i < 2; i++) {
        if (multiBalls[i].active) {
          for (int step = 0; step < PHYSICS_SUBSTEPS; step++) {
            updatePhysics(multiBalls[i]);
            checkCollisions(multiBalls[i]);
          }
          checkBallTransfer(multiBalls[i]);
          
          if (frameCount % 2 == 0) {
            multiBalls[i].trail[multiBalls[i].trailIdx][0] = multiBalls[i].x;
            multiBalls[i].trail[multiBalls[i].trailIdx][1] = multiBalls[i].y;
            multiBalls[i].trailIdx = (multiBalls[i].trailIdx + 1) % 6;
          }
        }
      }
      
      if (multiballActive) {
        bool anyActive = false;
        for (int i = 0; i < 2; i++) {
          if (multiBalls[i].active) anyActive = true;
        }
        if (!anyActive) multiballActive = false;
      }
    }
    
    // Balle ennemie: continue même en game over (jusqu'à ce qu'elle tombe ou sorte)
    if (enemyBall.active) {
      for (int step = 0; step < PHYSICS_SUBSTEPS; step++) {
        updatePhysics(enemyBall);
        checkCollisions(enemyBall);
      }
      checkBallTransfer(enemyBall);
      
      if (frameCount % 2 == 0) {
        enemyBall.trail[enemyBall.trailIdx][0] = enemyBall.x;
        enemyBall.trail[enemyBall.trailIdx][1] = enemyBall.y;
        enemyBall.trailIdx = (enemyBall.trailIdx + 1) % 6;
      }
    }
    
    for (int i = 0; i < NUM_TARGETS; i++) {
      if (targets[i].hit && now - targets[i].hitTime > 2500) targets[i].hit = false;
    }
    for (int i = 0; i < NUM_BUMPERS; i++) {
      if (bumpers[i].lit && now - bumpers[i].lastHit > 120) bumpers[i].lit = false;
    }
    for (int i = 0; i < NUM_SLINGSHOTS; i++) {
      if (slingshots[i].lit && now - slingshots[i].lastHit > 100) slingshots[i].lit = false;
    }
    
    drawGame();
  }
}

// ============================================
// DRAWING
// ============================================

void drawGame() {
  buffer.fillSprite(COL_BG);
  
  drawBackground();
  
  for (int i = 0; i < NUM_SLINGSHOTS; i++) drawSlingshot(slingshots[i]);
  for (int i = 0; i < NUM_TARGETS; i++) drawTarget(targets[i]);
  for (int i = 0; i < NUM_BUMPERS; i++) drawBumper(bumpers[i]);
  
  drawFlipper(leftFlipper);
  drawFlipper(rightFlipper);
  
  if (myBall.active) drawBall(myBall);
  if (enemyBall.active) drawBall(enemyBall);
  for (int i = 0; i < 2; i++) {
    if (multiBalls[i].active) drawBall(multiBalls[i]);
  }
  
  if (multiballActive) {
    uint16_t mbColor = (frameCount / 5) % 2 ? COL_GOLD : COL_NEON_PINK;
    buffer.setTextColor(mbColor);
    buffer.setTextSize(1);
    buffer.setCursor(35, GAME_TOP + 5);
    buffer.print("MULTIBALL!");
  }
  
  // Indicateur de position quand ma balle est chez l'ennemi
  if (myBallAtEnemy) {
    // Indicateur de position en haut (petite flèche)
    int indicatorX = (int)myBallEnemyPosX;
    if (indicatorX < 5) indicatorX = 5;
    if (indicatorX > SCREEN_W - 5) indicatorX = SCREEN_W - 5;
    
    // Animation
    int bounce = (frameCount / 4) % 3;
    int indicatorY = GAME_TOP + 5 + bounce;
    
    // Triangle pointant vers le bas
    buffer.fillTriangle(
      indicatorX, indicatorY + 6,
      indicatorX - 4, indicatorY,
      indicatorX + 4, indicatorY,
      COL_BALL_MINE
    );
    buffer.drawTriangle(
      indicatorX, indicatorY + 6,
      indicatorX - 4, indicatorY,
      indicatorX + 4, indicatorY,
      TFT_WHITE
    );
  }
  
  drawScoreBar();
  drawConnectionStatus();
  
  if (gameOver) {
    // Fond grisé complet
    for (int y = SCREEN_H/2 - 40; y < SCREEN_H/2 + 40; y++) {
      buffer.drawFastHLine(8, y, SCREEN_W - 16, COL_BG_DARK);
    }
    buffer.drawRoundRect(8, SCREEN_H/2 - 40, SCREEN_W - 16, 80, 8, COL_NEON_PINK);
    
    buffer.setTextColor(COL_NEON_PINK);
    buffer.setTextSize(2);
    buffer.setCursor(18, SCREEN_H/2 - 28);
    buffer.print("GAME OVER");
    
    buffer.setTextColor(TFT_WHITE);
    buffer.setTextSize(2);
    buffer.setCursor(20, SCREEN_H/2 - 2);
    buffer.print(score);
    
    buffer.setTextColor(COL_GOLD);
    buffer.setTextSize(1);
    buffer.setCursor(35, SCREEN_H/2 + 18);
    buffer.print("Appuyer");
    
  } else if (canLaunchBall() && ballsLeft > 0) {
    // Fond grisé pour le bandeau
    int bannerY = SCREEN_H / 2 - 20;
    for (int y = bannerY; y < bannerY + 40; y++) {
      buffer.drawFastHLine(8, y, SCREEN_W - 16, COL_BG_DARK);
    }
    buffer.drawRoundRect(8, bannerY, SCREEN_W - 16, 40, 6, COL_FLIPPER);
    
    buffer.setTextColor(TFT_WHITE);
    buffer.setTextSize(1);
    buffer.setCursor(22, bannerY + 8);
    buffer.print("Appuyer sur");
    buffer.setCursor(18, bannerY + 22);
    buffer.print("les 2 boutons");
  }
  
  buffer.pushSprite(0, 0);
}

void drawBackground() {
  for (int y = GAME_TOP; y < SCREEN_H; y += 20) {
    buffer.drawFastHLine(0, y, SCREEN_W, COL_BG_DARK);
  }
  for (int x = 0; x < SCREEN_W; x += 20) {
    buffer.drawFastVLine(x, GAME_TOP, GAME_H, COL_BG_DARK);
  }
  
  buffer.drawRect(0, GAME_TOP, SCREEN_W, GAME_H, COL_WALL);
  
  int peerCount = getActivePeerCount();
  if (peerCount > 0) {
    uint16_t transferCol = (frameCount / 10) % 2 ? COL_NEON_PINK : COL_WALL;
    buffer.drawFastHLine(20, GAME_TOP, SCREEN_W - 40, transferCol);
    buffer.drawFastHLine(20, GAME_TOP + 1, SCREEN_W - 40, transferCol);
  } else {
    buffer.drawFastHLine(0, GAME_TOP, SCREEN_W, COL_WALL_LIGHT);
  }
  
  float wallLeftX = 15;
  float wallRightX = SCREEN_W - 15;
  float wallTop = SCREEN_H - 80;
  float wallBottom = SCREEN_H - 35;
  
  for (int y = wallTop; y < wallBottom; y++) {
    uint16_t col = (y % 3 == 0) ? COL_WALL : COL_BG_DARK;
    buffer.drawFastHLine(0, y, wallLeftX, col);
    buffer.drawFastHLine(wallRightX, y, SCREEN_W - wallRightX, col);
  }
  
  buffer.drawLine(wallLeftX, wallTop, wallLeftX, wallBottom, COL_FLIPPER);
  buffer.drawLine(wallRightX, wallTop, wallRightX, wallBottom, COL_FLIPPER);
  buffer.drawLine(wallLeftX, wallBottom, leftFlipper.baseX, leftFlipper.baseY + 3, COL_FLIPPER);
  buffer.drawLine(wallRightX, wallBottom, rightFlipper.baseX, rightFlipper.baseY + 3, COL_FLIPPER);
}

void drawFlipper(Flipper &f) {
  float endX = f.baseX + cos(f.angle) * FLIPPER_LENGTH;
  float endY = f.baseY + sin(f.angle) * FLIPPER_LENGTH;
  
  float perpX = -sin(f.angle);
  float perpY = cos(f.angle);
  
  float baseWidth = FLIPPER_WIDTH * 0.8f;
  float tipWidth = FLIPPER_WIDTH * 0.25f;
  
  buffer.drawTriangle(
    f.baseX + perpX * (baseWidth + 2), f.baseY + perpY * (baseWidth + 2),
    f.baseX - perpX * (baseWidth + 2), f.baseY - perpY * (baseWidth + 2),
    endX, endY, COL_FLIPPER_DIM);
  
  int x0 = f.baseX + perpX * baseWidth;
  int y0 = f.baseY + perpY * baseWidth;
  int x1 = f.baseX - perpX * baseWidth;
  int y1 = f.baseY - perpY * baseWidth;
  int x2 = endX - perpX * tipWidth;
  int y2 = endY - perpY * tipWidth;
  int x3 = endX + perpX * tipWidth;
  int y3 = endY + perpY * tipWidth;
  
  buffer.fillTriangle(x0, y0, x1, y1, x2, y2, COL_FLIPPER);
  buffer.fillTriangle(x0, y0, x2, y2, x3, y3, COL_FLIPPER);
  buffer.drawLine(x0, y0, x3, y3, TFT_WHITE);
  buffer.fillCircle(endX, endY, tipWidth + 1, COL_FLIPPER);
  buffer.fillCircle(f.baseX, f.baseY, 5, TFT_WHITE);
  buffer.fillCircle(f.baseX, f.baseY, 4, COL_WALL_LIGHT);
  buffer.fillCircle(f.baseX, f.baseY, 2, TFT_WHITE);
}

void drawBumper(Bumper &b) {
  uint16_t mainCol = b.lit ? TFT_WHITE : b.color;
  uint16_t glowCol = b.lit ? b.color : b.glowColor;
  
  buffer.drawCircle((int)b.x, (int)b.y, b.radius + 3, glowCol);
  buffer.drawCircle((int)b.x, (int)b.y, b.radius + 2, glowCol);
  buffer.fillCircle((int)b.x, (int)b.y, b.radius, mainCol);
  buffer.drawCircle((int)b.x, (int)b.y, b.radius - 2, b.lit ? b.color : COL_BG_DARK);
  buffer.fillCircle((int)b.x, (int)b.y, 2, TFT_WHITE);
}

void drawTarget(Target &t) {
  if (!t.hit) {
    uint16_t col = ((frameCount / 8) % 2 == 0) ? COL_TARGET : COL_GOLD;
    buffer.fillRoundRect(t.x, t.y, t.w, t.h, 2, col);
    buffer.drawRoundRect(t.x, t.y, t.w, t.h, 2, TFT_WHITE);
    buffer.drawFastVLine(t.x + t.w/2, t.y + 2, t.h - 4, TFT_WHITE);
  } else {
    buffer.drawRoundRect(t.x, t.y, t.w, t.h, 2, COL_WALL);
  }
}

void drawSlingshot(Slingshot &s) {
  uint16_t col = s.lit ? TFT_WHITE : COL_NEON_PINK;
  uint16_t fillCol = s.lit ? COL_NEON_PINK : COL_BG_DARK;
  
  buffer.fillTriangle(s.x1, s.y1, s.x2, s.y2, s.x3, s.y3, fillCol);
  buffer.drawTriangle(s.x1, s.y1, s.x2, s.y2, s.x3, s.y3, col);
}

void drawBall(Ball &ball) {
  // Utiliser la couleur du propriétaire de la balle
  uint16_t ballColor = ball.color;
  // Le glow est la même couleur mais plus foncée (diviser par 2)
  uint16_t glowColor = ((ballColor >> 1) & 0x7BEF);  // Divise chaque composante par 2
  
  for (int i = 0; i < 6; i++) {
    int idx = (ball.trailIdx + i) % 6;
    if (i <= 2) {
      buffer.drawPixel(ball.trail[idx][0], ball.trail[idx][1], COL_WALL);
    } else if (i <= 4) {
      buffer.fillCircle(ball.trail[idx][0], ball.trail[idx][1], 1, glowColor);
    }
  }
  
  buffer.drawCircle((int)ball.x, (int)ball.y, BALL_RADIUS + 2, glowColor);
  buffer.fillCircle((int)ball.x, (int)ball.y, BALL_RADIUS, ballColor);
  buffer.fillCircle((int)ball.x - 1, (int)ball.y - 1, 1, TFT_WHITE);
}

void drawScoreBar() {
  for (int y = 0; y < SCORE_BAR_H; y++) {
    uint16_t col = (y < SCORE_BAR_H / 2) ? COL_SCORE_BG : COL_BG_DARK;
    buffer.drawFastHLine(0, y, SCREEN_W, col);
  }
  
  buffer.drawFastHLine(0, SCORE_BAR_H - 1, SCREEN_W, COL_FLIPPER);
  buffer.drawFastHLine(0, SCORE_BAR_H - 2, SCREEN_W, COL_FLIPPER_DIM);
  
  // Score à gauche - plus gros, sans "SCORE"
  buffer.setTextColor(score >= 0 ? TFT_WHITE : COL_BALL_ENEMY);
  buffer.setTextSize(2);
  buffer.setCursor(4, 4);
  if (score >= 0) {
    buffer.print(score);
  } else {
    buffer.print(score);
  }
  
  // Balles restantes au milieu-droite (décalé pour éviter le score)
  int ballsStartX = (SCREEN_W / 2) + 25 - ((ballsLeft * 11) / 2);
  for (int i = 0; i < ballsLeft; i++) {
    int bx = ballsStartX + i * 11;
    buffer.fillCircle(bx, 12, 4, myColor);
    buffer.drawCircle(bx, 12, 4, COL_FLIPPER);
  }
}

void drawConnectionStatus() {
  int peerCount = getActivePeerCount();
  int receivableCount = getReceivablePeerCount();
  
  uint16_t statusCol;
  if (peerCount == 0) {
    statusCol = COL_SEARCHING;
  } else {
    statusCol = COL_CONNECTED;
  }
  
  // Cercle de connexion à droite
  if (peerCount > 0 || (frameCount / 15) % 2) {
    buffer.fillCircle(SCREEN_W - 7, 12, 4, statusCol);
  }
  buffer.drawCircle(SCREEN_W - 7, 12, 4, TFT_WHITE);
  
  // Nombre de joueurs - plus gros
  buffer.setTextColor(statusCol);
  buffer.setTextSize(2);
  
  if (peerCount == 0) {
    // Mode solo - afficher "1" 
    buffer.setCursor(SCREEN_W - 22, 4);
    buffer.print("1");
  } else {
    // Afficher nombre total de joueurs
    buffer.setCursor(SCREEN_W - 22, 4);
    buffer.print(peerCount + 1);
  }
}
