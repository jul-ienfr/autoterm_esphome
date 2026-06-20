# 🔥 Autoterm UART Bridge pour ESPHome

Pont UART bidirectionnel entre les chauffages diesel **Autoterm/Planar** et leur panneau de commande, avec intégration native **ESPHome** et **Home Assistant**.

> [!WARNING]
> **Projet expérimental — utilisation à vos risques et périls !**
> Un mauvais câblage ou une configuration incorrecte peuvent **endommager le chauffage**.
> En cas de doute : **ne pas utiliser**.

---

## 📦 Fonctionnalités

- 🧭 **Bridge UART bidirectionnelle** entre panneau et chauffage
- 📊 **33+ capteurs** : températures, tension, pompe, carburant, usure, efficacité, diagnostic...
- 🌡️ **Climate avec presets** : chauffage, auto, ventilation, thermostat, mode puissance
- 🔴 **6 protections sécurité** : surtempérature, flameout, sous-tension, burn-out, lockout, watchdog
- 🟢 **Eco-Adaptive** ⭐ : mode continu sans on/off, feed-forward anti-dépassement
- 🟡 **PID Gain-Scheduled** : gains par phase (startup/approaching/steady) = -15-20% carburant
- 🟡 **Antifreeze zones** : 4 zones de puissance asymétriques au lieu de on/off
- 🟡 **Pre-heating par prédiction** : anticipe les chutes de température
- 🟡 **Exhaust feedback** : l'efficacité combustion ajuste le PID
- 🟡 **Maintenance prédictive** : seuils configurables, score d'usure brûleur
- 🟡 **Prédiction intelligente** : apprentissage des patterns horaires
- 🟢 **10 modes** : eco-adaptive, PID, hystérésis, puissance, ventilation, nuit, frost, economy, sleep, autonomous
- 🔌 **Protocole étendu** : diagnostic mode, unlock error 37, version firmware, historique, status report
- 🛡️ **Burn-out protection** : 4 min post-ignition sans arrêt
- 📊 **Fuel tracking** : consommation instantanée, totale, et quotidienne
- 🛰️ **Dashboard HTML** : interface web avec graphiques temps réel
- 🏠 **19 automatisations HA** : alertes, maintenance, confort, énergie, lockout, eco-adaptive
- 🔧 **Script Arduino** : test UART avec 11 commandes

---

## 📁 Structure du projet

```
Projet_Autoterm/
├── components/autoterm_uart/
│   ├── __init__.py              ← Registration ESPHome (380+ lignes)
│   └── autoterm_uart.h          ← Composant C++ complet (3900+ lignes)
├── air4d.yaml                   ← Config ESPHome production
├── air4d_test.yaml              ← Config test diagnostic UART
├── secrets.yaml                 ← WiFi + mots de passe (à remplir)
├── dashboard.html               ← Interface web temps réel
├── ha_automations.yaml          ← 19 automatisations Home Assistant
├── test_uart.ino                ← Script Arduino test UART
├── README.md                    ← Ce fichier
├── INSTALLATION.md              ← Guide pas-à-pas d'installation
├── WIRING.md                    ← Guide de câblage
├── modie.md                     ← Référence protocole UART
└── img/                         ← Captures d'écran
```

---

## 🔴 Sécurité (5 protections)

| Protection | Seuil | Action |
|------------|-------|--------|
| Surtempérature échappement | > 400°C | Arrêt immédiat |
| Flameout (pompe active + T basse) | < 100°C / 60s | Coupe carburant |
| Tension démarrage basse | < 10V pendant ignition | Alerte ERROR |
| Tension critique opération | < 9.0V | Arrêt immédiat |
| Watchdog matériel | 30s | Redémarrage auto |

---

## 🟢 Eco-Adaptive Mode ⭐

Mode de chauffage recommandé. Module la puissance en continu (niveaux 1-9) sans jamais éteindre/reallumer le chauffage.

```yaml
autoterm_uart:
  eco_adaptive: true
  eco_kp: 1.5          # Proportionnel (plus doux que PID)
  eco_ki: 0.3          # Intégral (apprentissage de la pièce)
  eco_kd: 0.2          # Dérivée (anticipation)
  eco_min_level: 1     # Niveau minimum
  eco_max_level: 9     # Niveau maximum
  eco_deadband: 0.3    # Zone morte ±0.3°C
  eco_overshoot_predict: true  # Anti-dépassement par feed-forward
```

**Avantages** :
- 🛢️ Moins de carburant (pas de cycles arrêt/démarrage)
- ⚡ Moins d'électricité (moins de démarrages = moins d'usage bougie)
- 🔇 Moins de bruit (niveaux bas continus vs niveaux hauts intermittents)
- 🔧 Moins d'usure (pas de stress thermique par cycles)
- 🌡️ Température plus stable (±0.3°C vs ±3°C en hystérésis)

**Comment ça marche** : Le contrôleur calcule en permanence le niveau optimal basé sur l'erreur de température, sa vitesse de changement, et un modèle thermique appris. Le feed-forward prévient les dépassements en réduisant la puissance quand la température monte vite vers la cible.

---

## 🟡 PID Controller (alternative)

```yaml
autoterm_uart:
  pid_mode: true
  pid_kp: 2.0    # Proportional (réactivité)
  pid_ki: 0.5    # Integral (corrige l'erreur statique)
  pid_kd: 0.1    # Derivative (anticipe les changements)
```

Module la puissance (niveaux 1-9) proportionnellement à l'erreur de température. Réduit les oscillations et économise carburant.

---

## 🟡 Capteurs (33+)

| Capteur | Unité | Description |
|---------|-------|-------------|
| `internal_temperature` | °C | T° interne chauffage |
| `external_temperature` | °C | Sonde externe (avec fallback cache) |
| `heater_temperature` | °C | T° échappement |
| `panel_temperature` | °C | T° panneau |
| `voltage` | V | Tension batterie |
| `fan_speed_set` / `actual` | rpm | Ventilateur consigne/réel |
| `pump_frequency` | Hz | Fréquence pompe |
| `fuel_consumption` | L/h | Conso instantanée |
| `total_fuel_consumed` | L | Conso totale cumulée |
| `daily_fuel_consumed` | L | Conso quotidienne (reset minuit) |
| `combustion_efficiency` | % | Efficacité combustion |
| `delta_t` | °C | T° échappement - T° ambiante |
| `ignition_time` | s | Temps d'allumage |
| `wear_score` | % | Score usure brûleur |
| `fuel_economy_savings` | % | Temps près de la cible |
| `predicted_temp` | °C | Température prédite |
| `boot_count` | — | Nombre de boots ESP32 |
| `free_heap` | B | Mémoire libre |
| `error_code` | — | Code erreur (byte 2 status) |
| `total_starts` | — | Nombre total de démarrages |
| `glow_plug_current` | % | Courant bougie préchauffage (diagnostic) |
| `chamber_temp` | °C | T° chambre combustion (diagnostic) |
| `board_temp` | °C | T° carte ECU (diagnostic) |

### Capteurs optionnels (hardware externe)

> ⚠️ **Le chauffage Autoterm a déjà des capteurs intégrés** (T° échappement, interne, externe, chambre combustion, tension, ventilateur, pompe) qui remontent via le protocole UART. Les capteurs ci-dessous sont un **PLUS** pour améliorer le système, pas une nécessité.

| Capteur | Prix | Hardware nécessaire | Quand activer |
|---------|------|---------------------|---------------|
| **CO Level** (🔴 sécurité) | ~3€ | MQ-7 sur GPIO34 | **Recommandé** — détecte fuites monoxyde de carbone |
| **GPS Altitude** | 0€ | HA Companion App avec GPS | En altitude > 1000m (compensation auto) |
| **Exhaust Temp Direct** | ~5€ | Thermocouple K + MAX6675 sur SPI | Plus de précision que la T° UART |

**Tous fonctionnent SANS hardware** — le système se dégrade gracieusement si le capteur n'est pas là.

---

## 🟢 Modes

| Mode | Description |
|------|-------------|
| **Eco-Adaptive** ⭐ | Mode adaptatif continu : module la puissance sans cycles on/off. Feed-forward, exhaust feedback. **Recommandé.** |
| **PID Gain-Scheduled** | 3 phases (startup/approaching/steady) avec gains adaptatifs |
| **Hystérésis** | On/off intelligent avec niveau proportionnel |
| **Nuit** | Réduit puissance + ventilateur (moins de bruit) |
| **Frost Protection** | 4 zones asymétriques : OFF/1-2/3-5/6-9 selon T° ext |
| **Pre-heating** | Anticipe les chutes de T° par prédiction horaire |
| **Fuel Economy** | Tracking + réduction proactive |
| **Light Sleep** | Réduit consommation en veille |
| **Burn-out** | Protection 4 min post-ignition |
| **Autonomous** | Fonctionne sans panneau physique |

---

## 🏠 Entités Home Assistant (50+)

| Type | Nom | Description |
|------|-----|-------------|
| Climate | Heating | Thermostat complet avec presets |
| Sensor | 33+ capteurs | Voir tableau ci-dessus |
| Text Sensor | error_text, firmware_version, error_log, eco_mode_status | Infos texte |
| Switch | Night Mode | Mode nuit on/off |
| Switch | Frost Protection | Protection anti-gel on/off |
| Switch | Debug Mode | Logs DEBUG à chaud |
| Button | Restart ESP | Redémarrage ESP32 |
| Button | Safe Mode | Mode sûr pour récupération |
| Button | Clear Lockout | Déverrouille error 37 + état urgence |
| Button | Prime Fuel Pump | Amorce pompe carburant |
| Button | Status Report | Snapshot diagnostic complet en 1 clic |
| Select | Temperature Source | Source de régulation (Int/Panel/Ext/HA) |

---

## 🔧 Modes de chauffage

- **Mode puissance** : Fonctionnement open loop au niveau choisi (0-9)
- **Chauffage** : Régulation par paliers jusqu'à la consigne
- **Chauffage + Ventilation** : Hybride — chauffe puis ventilation seule à la consigne
- **Ventilation seule** : Brûleur éteint, ventilateur actif
- **Thermostat** : Puissance avec hystérésis configurable (on/off intelligent)
- **PID** : Régulation proportionnelle-intégrale-dérivée (nouveau)

---

## ⚙️ Configuration rapide

### 1. Créer `secrets.yaml`
```yaml
wifi_ssid: "TonWiFi"
wifi_password: "TonMotDePasse"
fallback_password: "AutotermFallback"
web_username: "admin"
web_password: "TonMotDePasse"
```

### 2. Compiler et flasher
Voir [INSTALLATION.md](INSTALLATION.md) pour le guide complet.

### 3. Options dans `air4d.yaml`
```yaml
autoterm_uart:
  # Sécurité (activée par défaut)
  frost_protection: true
  frost_protection_temp: 2.0

  # PID (désactivé par défaut)
  pid_mode: false
  pid_kp: 2.0
  pid_ki: 0.5
  pid_kd: 0.1

  # Maintenance (seuils configurables)
  maintenance_oil_hours: 500.0
  maintenance_filter_hours: 200.0
  maintenance_glow_hours: 1000.0

  # Modes avancés (désactivés par défaut)
  night_mode: false
  fuel_economy: false
  fuel_economy_reactive: false
  prediction: false
  light_sleep: false
```

---

## 📄 Documentation

- [INSTALLATION.md](INSTALLATION.md) — Guide pas-à-pas d'installation
- [WIRING.md](WIRING.md) — Guide de câblage et pinout
- [modie.md](modie.md) — Référence du protocole UART
- [ha_automations.yaml](ha_automations.yaml) — Automatisations HA prêtes (19 automations)
- [dashboard.html](dashboard.html) — Interface web temps réel
- [test_uart.ino](test_uart.ino) — Script Arduino pour débugger le hardware

### Documentation externe recommandée

- [kalutep/serial_communication_protocol.md](https://github.com/kalutep/AutotermHeaterController/blob/main/serial_communication_protocol.md) — Spec complète du protocole (status codes, error codes, diagnostic telemetry)
- [schroeder-robert/README.md](https://github.com/schroeder-robert/autoterm-air-2d-serial-control) — Documentation originale du protocole

---

## 📡 MQTT & Grafana (optionnel)

Le composant publie automatiquement tous les capteurs via MQTT (configuré dans `air4d.yaml`).

### Setup rapide

1. **Broker MQTT** : Installer Mosquitto ou utiliser le broker HA intégré
2. **InfluxDB** : Créer une base `autoterm` avec le plugin MQTT Telegraf
3. **Grafana** : Ajouter InfluxDB comme source, créer les tableaux de bord

### Topics MQTT publiés

```
autoterm/sensor/internal_temperature/state
autoterm/sensor/voltage/state
autoterm/sensor/fuel_consumption/state
autoterm/sensor/eco_adaptive_level/state
autoterm/sensor/eco_power_efficiency/state
... (25+ capteurs)
```

### Exemple dashboard Grafana

```json
{
  "panels": [
    {"title": "Températures", "targets": [{"query": "SELECT mean(\"value\") FROM \"autoterm\" WHERE \"entity_id\" =~ /^internal_temperature|external_temperature|heater_temperature$/"}]},
    {"title": "Consommation", "targets": [{"query": "SELECT mean(\"value\") FROM \"autoterm\" WHERE \"entity_id\" = 'fuel_consumption'"}]},
    {"title": "Niveau Eco-Adaptive", "targets": [{"query": "SELECT last(\"value\") FROM \"autoterm\" WHERE \"entity_id\" = 'eco_adaptive_level'"}]}
  ]
}
```

---

## 🧑‍💻 Développement & tests

- ESP32 DevKit v1
- Autoterm Air 2D / 4D
- ESPHome 2025.x / Home Assistant 2025.x

---

## 📚 Sources & Références

Ce projet s'appuie sur le travail de reverse-engineering et d'implémentation de plusieurs projets open-source :

### Protocole UART

| Projet | Contribution |
|--------|-------------|
| [schroeder-robert/autoterm-air-2d-serial-control](https://github.com/schroeder-robert/autoterm-air-2d-serial-control) | Documentation originale du protocole UART (13 commandes, frame structure, CRC16, status codes). Source principale pour les commandes `0x01`-`0x23`. |
| [kalutep/AutotermHeaterController](https://github.com/kalutep/AutotermHeaterController) | Documentation protocol étendue avec captures de messages. Source pour le **mode diagnostic** (`0x07`), les **codes d'erreur** (17 codes documentés), l'**historique** (`0x0B`), et le format du flux de 72 octets. |

### Implémentation ESPHome / Home Assistant

| Projet | Contribution |
|--------|-------------|
| [tim0816/autoterm_esphome](https://github.com/tim0816/autoterm_esphome) | Composant ESPHome avec climate entity, bridge UART MITM, mode autonome. Référence pour l'architecture du bridge et la synchronisation état panel/HA. |
| [hutterm/Autoterm-Air-2D-HACS](https://github.com/hutterm/Autoterm-Air-2D-HACS) | Intégration HA native (custom component). Référence pour le **fallback température externe** et la gestion des capteurs indisponibles. |

### Autres

| Projet | Contribution |
|--------|-------------|
| [AYeropkin/autoterm](https://github.com/AYeropkin/autoterm) | Reverse engineering du bus CAN pour intégration Victron GX. |
| [k3mpaxl/pekaway-ha-autoterm](https://github.com/k3mpaxl/pekaway-ha-autoterm) | Intégration HA alternative. |
| [esdete2/dbus-autoterm](https://github.com/esdete2/dbus-autoterm) | Plugin D-Bus pour Venus OS (Victron GX). |

### Documentation protocole

La documentation la plus complète se trouve dans le repo **kalutep** :
- [`serial_communication_protocol.md`](https://github.com/kalutep/AutotermHeaterController/blob/main/serial_communication_protocol.md) — Spec complète avec tous les status codes, error codes, et le format du diagnostic telemetry (72 octets)
- [`messages/`](https://github.com/kalutep/AutotermHeaterController/tree/main/messages) — Captures de messages bruts
- [`message_captures/`](https://github.com/kalutep/AutotermHeaterController/tree/main/message_captures) — Logs du mode diagnostic avec le logiciel Autoterm Test v1.11

---

## 📄 Licence

MIT License © 2025 — Développé par **Tim**
