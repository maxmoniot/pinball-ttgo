# 🕹️ Neon Pinball Multiplayer

Un jeu de flipper néon multijoueur pour TTGO T-Display ESP32, utilisant ESP-NOW pour la communication sans fil entre les appareils.

![Version](https://img.shields.io/badge/version-1.0-blue)
![Platform](https://img.shields.io/badge/platform-ESP32-green)
![License](https://img.shields.io/badge/license-MIT-orange)

## 📖 Description

Neon Pinball est un jeu de flipper rétro avec une esthétique néon, jouable en solo ou en multijoueur (2 à 10 joueurs). Chaque joueur utilise son propre TTGO T-Display et les balles peuvent voyager d'un écran à l'autre via la communication ESP-NOW !

## ✨ Fonctionnalités

- **Mode Solo** : Jouez seul et tentez de battre votre meilleur score
- **Mode Multijoueur** : 2 à 10 joueurs, connexion automatique via ESP-NOW
- **Multiball** : Déclenchez le multiball en atteignant 2000 points (multi) ou 3000 points (solo) avec une seule balle
- **Transfert de balles** : Les balles sortant par le haut vont chez un adversaire aléatoire
- **Couleurs uniques** : Chaque joueur a sa propre couleur de balle
- **Éléments de jeu** : Bumpers, cibles, slingshots, flippers réactifs
- **Deep Sleep** : Option d'économie de batterie avec interrupteur externe (optionnel)

## 🎮 Règles du jeu

### Objectif
Marquer un maximum de points en touchant les bumpers, cibles et autres éléments du flipper.

### Multijoueur
- Les balles qui sortent par le **haut** de l'écran sont envoyées à un adversaire aléatoire
- Quand une balle ennemie est chez vous, vous pouvez marquer des points pour son propriétaire
- Si une balle ennemie sort par le haut, elle **retourne à son propriétaire**
- Si une balle ennemie tombe en bas, son propriétaire peut la relancer (sans perdre de vie)

### Perte de vie
- Vous perdez une vie **uniquement** si votre balle tombe en bas **chez vous**
- Pendant un multiball, vous ne perdez une vie que lorsque **toutes** vos balles sont tombées
- 3 vies par partie

### Points
| Élément | Points |
|---------|--------|
| Bumper | 100 |
| Cible | 250 |
| Slingshot | 50 |

## 🔧 Matériel requis

- **TTGO T-Display ESP32** (un par joueur)
- Câble USB pour la programmation
- (Optionnel) Interrupteur sur GPIO 13 pour le deep sleep
- (Optionnel) Batterie LiPo

## 📦 Installation

### Prérequis

1. [Arduino IDE](https://www.arduino.cc/en/software) (1.8.x ou 2.x)
2. [Support ESP32 pour Arduino](https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html)
3. Bibliothèque **TFT_eSPI** (via le gestionnaire de bibliothèques Arduino)

### Configuration de TFT_eSPI

Modifiez le fichier `User_Setup_Select.h` de la bibliothèque TFT_eSPI :

```cpp
// Commentez la configuration par défaut
// #include <User_Setup.h>

// Décommentez la configuration TTGO T-Display
#include <User_Setups/Setup25_TTGO_T_Display.h>
```

### Téléversement

1. Clonez ce dépôt :
```bash
git clone https://github.com/votre-username/neon-pinball-multiplayer.git
```

2. Ouvrez `pinball_multi.ino` dans Arduino IDE

3. Sélectionnez la carte : **ESP32 Dev Module** ou **TTGO T-Display**

4. Sélectionnez le port COM approprié

5. Téléversez !

## ⚙️ Configuration

### Options au début du code

```cpp
// === OPTION DEEP SLEEP ===
// Mettre à true pour activer le deep sleep avec l'interrupteur sur GPIO 13
// Mettre à false pour désactiver (l'ESP reste toujours allumé)
#define ENABLE_DEEP_SLEEP false

// Seuils multiball
#define MULTIBALL_THRESHOLD_MULTI 2000  // Mode multi: 2000 points
#define MULTIBALL_THRESHOLD_SOLO  3000  // Mode solo: 3000 points

// Timeout balle chez l'ennemi
#define BALL_AT_ENEMY_TIMEOUT 2000  // 2 secondes
```

### Branchement Deep Sleep (optionnel)

```
GPIO 13 ──┬── Interrupteur ──┬── 3.3V
          │                  │
          └── (position OFF) ┘
```

- Position OFF (GPIO 13 = LOW) → Deep sleep (~10µA)
- Position ON (GPIO 13 = HIGH) → Fonctionnement normal

## 🎯 Contrôles

| Bouton | Action |
|--------|--------|
| **Gauche (GPIO 0)** | Flipper gauche |
| **Droite (GPIO 35)** | Flipper droit |
| **Les deux (maintenu)** | Lancer la balle / Nouvelle partie |

## 🌐 Communication ESP-NOW

Le jeu utilise ESP-NOW pour la communication entre les appareils :

- **Canal WiFi** : 1 (configurable via `WIFI_CHANNEL`)
- **Découverte automatique** : Les appareils se découvrent via des pings broadcast
- **Timeout peer** : 3 secondes sans réponse = déconnexion
- **Messages** :
  - Type 0 : PING (découverte + keepalive)
  - Type 1 : BALL_SEND (transfert de balle)
  - Type 2 : BALL_LOST (notification balle tombée)
  - Type 3 : BALL_POSITION (position de la balle chez l'ennemi)
  - Type 4 : BALL_SCORE (points marqués)

## 📁 Structure du projet

```
neon-pinball-multiplayer/
├── pinball_multi.ino    # Code source principal
├── README.md            # Ce fichier
└── LICENSE              # Licence MIT
```

## 🎨 Palette de couleurs des joueurs

| Rang | Couleur |
|------|---------|
| 1 | Cyan |
| 2 | Magenta |
| 3 | Vert |
| 4 | Jaune |
| 5 | Rouge |
| 6 | Bleu |
| 7 | Orange |
| 8 | Gris clair |
| 9 | Rose |
| 10 | Cyan clair |

Les couleurs sont attribuées automatiquement en fonction du rang (basé sur l'ID unique de chaque ESP32).

## 🐛 Dépannage

### Les appareils ne se connectent pas
- Vérifiez que tous les appareils utilisent le même canal WiFi (`WIFI_CHANNEL`)
- Rapprochez les appareils (portée ESP-NOW : ~200m en extérieur, moins en intérieur)
- Redémarrez les appareils

### L'écran reste noir
- Vérifiez que `ENABLE_DEEP_SLEEP` est sur `false`
- Si vous utilisez le deep sleep, vérifiez l'interrupteur sur GPIO 13

### La balle disparaît
- Un timeout de 2 secondes permet de récupérer une balle "perdue"
- Attendez le message "Appuyer sur les 2 boutons"

## 📝 Changelog

### Version 1.0
- Jeu de flipper complet avec physique réaliste
- Mode multijoueur 2-10 joueurs via ESP-NOW
- Système multiball
- Couleurs uniques par joueur
- Option deep sleep
- Anti-blocage avec timeout

## 🤝 Contribution

Les contributions sont les bienvenues ! N'hésitez pas à :
- Signaler des bugs via les Issues
- Proposer des améliorations via les Pull Requests
- Partager vos idées

## 📄 Licence

Ce projet est sous licence MIT. Voir le fichier [LICENSE](LICENSE) pour plus de détails.

## 👨‍🏫 Auteur

Projet éducatif développé pour l'enseignement de la programmation et de l'électronique.

---

⭐ Si ce projet vous plaît, n'hésitez pas à lui donner une étoile sur GitHub !
