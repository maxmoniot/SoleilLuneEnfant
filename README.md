# 🌞🌙 SunMoon - Veilleuse Jour/Nuit pour Enfants

Une veilleuse intelligente basée sur TTGO T-Display (ESP32) qui aide les enfants à savoir quand ils peuvent se lever le matin ou quand il faut se rendormir.

![Soleil](https://img.shields.io/badge/Soleil-Il%20est%20l'heure%20de%20se%20lever!-yellow)
![Lune](https://img.shields.io/badge/Lune-Il%20faut%20encore%20dormir-blue)

## 🎯 Concept

Les jeunes enfants ne savent pas encore lire l'heure. Cette veilleuse leur donne une indication visuelle simple :

- **☀️ Soleil affiché** → "Tu peux te lever !"
- **🌙 Lune affichée** → "Il faut encore dormir"

Les heures de lever et de coucher sont configurables par les parents.

## 📷 Aperçu

| Mode Jour | Mode Nuit |
|-----------|-----------|
| Soleil lumineux | Lune très sombre |
| Backlight à 100% | Backlight éteint |

## 🔧 Matériel requis

- **TTGO T-Display V1** (ESP32 avec écran TFT 1.14" intégré)
- Batterie LiPo (optionnel, pour fonctionnement sans fil)
- Câble USB-C pour la programmation et la charge

## 📦 Installation

### 1. Prérequis Arduino IDE

Installer les bibliothèques suivantes :
- **TFT_eSPI** (par Bodmer)
- **ESP32 Board** (via le gestionnaire de cartes)

### 2. Configuration de TFT_eSPI

Dans le fichier `User_Setup_Select.h` de la bibliothèque TFT_eSPI, commenter toutes les configurations et décommenter :
```cpp
#include <User_Setups/Setup25_TTGO_T_Display.h>
```

### 3. Préparer les images

Créer deux fichiers d'en-tête pour les images :
- `soleilimg.h` - Image du soleil (135x240 pixels, format RGB565)
- `luneimg.h` - Image de la lune (135x240 pixels, format RGB565)

Utiliser un convertisseur d'images comme [image2cpp](http://javl.github.io/image2cpp/) avec les paramètres :
- Canvas size: 135x240
- Output format: Arduino code, single bitmap
- Draw mode: Horizontal

### 4. Configuration WiFi

Modifier les identifiants WiFi dans le code :
```cpp
const char* ssid = "VotreSSID";
const char* password = "VotreMotDePasse";
```

### 5. Téléverser

- Carte : "TTGO T-Display"
- Port : sélectionner le port COM approprié
- Téléverser le sketch

## 🎮 Utilisation

### Boutons

| Bouton | Action courte | Action longue |
|--------|---------------|---------------|
| **Jaune** (gauche) | Affiche les réglages actuels (2s) | - |
| **Vert** (droite) | Affiche le niveau de batterie (2s) | - |
| **Jaune + Vert** | - | Menu de réglage des heures (3s) |
| **Interrupteur OFF** | Mise en veille profonde | - |

### Menu de réglages

1. Maintenir les deux boutons (Jaune + Vert) pendant 3 secondes
2. Le champ actif clignote
3. **Bouton Jaune** : Incrémenter la valeur (maintenir pour défiler)
4. **Bouton Vert** : Passer au champ suivant
5. À la fin, confirmer (Jaune = Oui) ou annuler (Vert = Non)

### Réglages configurables

- **Heure de lever du soleil** : Heure à laquelle le soleil s'affiche
- **Heure de coucher du soleil** : Heure à laquelle la lune s'affiche

## ⚡ Caractéristiques techniques

- **Synchronisation horaire** : NTP automatique via WiFi au démarrage
- **Persistance** : Les réglages sont conservés même après extinction (mémoire RTC)
- **Deep Sleep** : Consommation minimale en veille, réveil par interrupteur
- **Anti-brownout** : Détection des redémarrages sur batterie faible
- **Luminosité adaptative** :
  - Jour : 100% (soleil bien visible)
  - Nuit : 0% (backlight éteint pour ne pas déranger le sommeil)

## 🔋 Autonomie

- En fonctionnement : ~8-12h selon la batterie
- En deep sleep : plusieurs semaines

## 📁 Structure des fichiers

```
SunMoon/
├── SunMoon.ino        # Code principal
├── soleilimg.h        # Image du soleil (à créer)
├── luneimg.h          # Image de la lune (à créer)
└── README.md          # Ce fichier
```

## 🐛 Dépannage

### L'écran reste noir
- Vérifier que les images sont correctement converties en RGB565
- Vérifier la configuration de TFT_eSPI

### Boucle de redémarrage sur batterie
- Normal si la batterie est trop faible pour le WiFi
- Le système détecte le brownout et utilise l'heure interne

### L'heure est incorrecte
- Vérifier la connexion WiFi
- Les constantes `gmtOffset_sec` et `daylightOffset_sec` sont configurées pour la France (GMT+1 avec heure d'été)

### Flash blanc au démarrage
- Comportement normal du contrôleur ST7789, minimisé autant que possible

## 📄 Licence

Ce projet est sous licence MIT. Libre à vous de l'utiliser, le modifier et le partager.

## 🙏 Crédits

- Bibliothèque [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) par Bodmer
- Basé sur l'ESP32 et le TTGO T-Display

---

*Fait avec ❤️ pour aider les enfants (et leurs parents) à mieux dormir*
