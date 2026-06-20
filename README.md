# 🔥 Bridge UART Autoterm pour ESPHome

Ce projet implémente une **bridge UART bidirectionnelle** entre les chauffages Autoterm/Planar et leur panneau de commande, avec intégration native dans **ESPHome** (donc Home Assistant).
Il permet de **surveiller et piloter le chauffage** via Wi‑Fi, MQTT ou les entités Home Assistant.

---

> [!WARNING]
> **Projet expérimental – utilisation à vos risques et périls !**
> Ce dépôt est **encore en développement**. Un mauvais câblage, une configuration incorrecte ou un comportement imprévu du firmware peuvent **endommager le chauffage**.
>
> En cas de doute : **ne pas utiliser**.

---

## 📦 Fonctionnalités

- 🧭 **Bridge UART bidirectionnelle** entre l’afficheur (panneau) et le chauffage, avec relay de toutes les trames
- 📊 **Capteurs** : température interne/externe/chauffage/panneau, tension d’alimentation, code/texte d’état, vitesses ventilateur (consigne/mesurée) et fréquence de pompe
- 🌡️ **Entité Climate avec presets** : contrôle chauffage, auto, ventilation et niveaux de puissance via ESPHome/Home Assistant
- 🎚️ **Réglages directs** : entité Number pour le niveau ventilateur et entité Select pour la source de température (incluant un feed « Home Assistant »)
- 🛰️ **Panneau virtuel** : option d’override injectant une température externe dans le flux de données « panel »
- 🧩 **Intégration Home Assistant** via composants ESPHome natifs
- 🧾 **Logs détaillés** des trames UART (HEX) en niveau DEBUG
- ⚙️ **Mode autonome** : requêtes status/settings automatiques si aucun panneau n’est détecté

---

## Captures d’écran
<img src="img/Screenshot_Heizen.png" width="300"><img src="img/Screenshot_HeizenLueften.png" width="300"><img src="img/Screenshot_Leistungmodus.png" width="300">

---

## 🔥 Modes de chauffage (détails)

- **Mode puissance**
  Fonctionnement « open loop » : le chauffage tourne uniquement au niveau choisi (`0–9`) et ignore les températures cibles. Pratique pour chauffer rapidement ou maintenir une forte puissance.

- **Chauffage**
  Régulation par paliers jusqu’à la consigne : le chauffage utilise la source de température choisie, monte en puissance jusqu’à la consigne puis continue en palier minimal.

- **Chauffage + Ventilation**
  Mode hybride : démarre en chauffage puis bascule en ventilation seule quand la consigne est atteinte. Si la température redescend, il repasse automatiquement en chauffage. En interne : `wait_mode = 0x01`.

- **Ventilation seule**
  Équivalent du mode « Ventiler uniquement » du panneau d’origine. Le brûleur reste éteint, seul le ventilateur tourne au niveau choisi (`0–9`).

- **Thermostat**
  Mode puissance avec hystérésis configurable : le chauffage tourne au niveau choisi jusqu’à dépasser la bande haute `SET + Hys_off`. Il lance ensuite un cycle de refroidissement (temporairement `SET − 5 °C`, `wait_mode = 0x01`). Dès que le statut « ventilation post‑refroidissement » est atteint, une commande Standby est envoyée et le brûleur reste éteint jusqu’à ce que la température repasse sous `SET − Hys_on`.

Chaque mode peut être piloté via l’entité Climate ou automatisé via ESPHome/Home Assistant. Après un changement de preset, le firmware met à jour ses paramètres internes et envoie les trames UART correspondantes au chauffage.

---

## ⚙️ Exemple de configuration

Un exemple complet est fourni dans **`air4d.yaml`**.
Il montre comment intégrer la composante Autoterm UART dans ESPHome.
Adapte impérativement le fichier à ton **câblage, tes GPIOs et ton matériel**.

> 📌 **Pour le câblage complet**, voir [WIRING.md](WIRING.md) (pinout, conversion de niveau logique, test de validation).

### Config de test (diagnostic UART)

Un fichier **`air4d_test.yaml`** est disponible pour diagnostiquer les problèmes de communication UART :
- Framework **Arduino** (au lieu d'ESP-IDF)
- Logger en **DEBUG complet**
- Utile pour vérifier si le câblage fonctionne avant de passer en production

### Config principale (production)

La config optimisée se trouve dans **`Autoterme_Optimiser/air4d.yaml`** avec :
- Framework ESP-IDF (unicore, 240MHz)
- Filtres capteurs, alerte tension, debug mode toggle
- Web server avec authentification

Pour le mode thermostat, l’hystérésis se définit directement dans le bloc Climate :

```yaml
climate:
  id: autoterm_climate
  thermostat_hysteresis_on: 2.0     # allumer quand Temp < SET - 2 °C
  thermostat_hysteresis_off: 1.0    # couper quand Temp > SET + 1 °C
```

Plages autorisées : `1–5 °C` (Hys_on) et `0–2 °C` (Hys_off).

---

## 🧩 Entités Home Assistant

| Type | Nom (par défaut) | Description |
|------|-------------------|-------------|
| Climate | Chauffage Autoterm | Entité Climate complète (modes, presets, consigne) |
| Sensor | Température interne | Température interne du chauffage (°C) |
| Sensor | Température externe | Sonde externe (°C) |
| Sensor | Température chauffage | Température échangeur (°C) |
| Sensor | Température panneau | Température panneau/afficheur (°C, réelle ou virtuelle) |
| Sensor | Tension | Tension d’alimentation (V) |
| Sensor | Ventilateur consigne | Vitesse ventilateur demandée (rpm) |
| Sensor | Ventilateur réel | Vitesse ventilateur mesurée (rpm) |
| Sensor | Fréquence pompe | Fréquence de la pompe doseuse (Hz) |
| Text Sensor | Statut | Statut en clair (avec fallback HEX si inconnu) |
| Select | Source température | Source (Interne/Panneau/Externe/Home Assistant) |

Pour l’override de température « panneau », tu peux lier un capteur existant (par ex. depuis Home Assistant) et le référencer dans `panel_temp_override.sensor`. Il sera utilisé quand la source « Home Assistant » est sélectionnée.

---

## 🧠 Détails du protocole UART

Chaque message (trame) a la structure suivante :

| Index | Signification | Exemple | Description |
|------:|--------------|---------|-------------|
| 0 | Start | `0xAA` | Début de trame |
| 1 | ID appareil | `0x03` / `0x04` | `0x03` = vers chauffage, `0x04` = réponse chauffage |
| 2 | Longueur payload | ex. `0x13` | Nombre d’octets entre header et CRC |
| 3 | ? | `0x00` | – |
| 4 | Code fonction | `0x0F`, `0x02`, `0x03`, … | Type de message |
| 5 … N−2 | Données | – | Variable |
| N−2, N−1 | CRC | ex. `0x3A 0E` | CRC16 |

Le calcul CRC est de type Modbus (voir sources).

---

## 🧑‍💻 Dév & tests

Testé avec :

- **ESP32 DevKit v1**
- **Autoterm Air 2D**
- Sniffer UART / logs pour analyser le protocole
- CRC16 Modbus
- ESPHome 2025.x / Home Assistant 2025.x

---

## 📚 Sources & références

- 🔗 [schroeder-robert / autoterm-air-2d-serial-control](https://github.com/schroeder-robert/autoterm-air-2d-serial-control)
  Reverse engineering et contrôle de l’Autoterm Air 2D via liaison série.

---

## 📄 Licence

MIT License © 2025
Développé par **Tim**
