# 🛠️ Guide d'installation — Autoterm UART Bridge ESPHome

Guide pas-à-pas pour installer et configurer le pont UART Autoterm avec ESPHome et Home Assistant.

---

## 📦 Matériel nécessaire

| Composant | Quantité | Prix estimé | Lien |
|-----------|----------|-------------|------|
| ESP32 DevKit v1 | 1 | ~5€ | AliExpress / Amazon |
| Convertisseur 5V→3.3V (MAX3232 ou diviseur) | 2 | ~1€ | AliExpress |
| Câbles Dupont M/F | 10 | ~2€ | AliExpress |
| Câbles Dupont F/F | 4 | ~1€ | AliExpress |
| Alimentation 12V→5V buck converter | 1 | ~3€ | AliExpress |
| Boîtier 3D (optionnel) | 1 | ~5€ | Thingiverse |

**Total de base: ~15-20€**

### Capteurs optionnels (recommandés)

> **Ce que le fabricant recommande** : L'Autoterm Air 4D est conçu pour fonctionner avec ses capteurs internes (T° échappement, interne, externe, chambre combustion, tension, ventilateur, pompe). Ces capteurs remontent via le protocole UART et sont **suffisants** pour un usage normal. Les capteurs ci-dessous sont des **améliorations** pour les utilisateurs avancés qui veulent plus de sécurité ou de précision.

> ⚠️ **Le chauffage Autoterm a déjà des capteurs intégrés** (T° échappement, interne, externe, chambre combustion, tension, ventilateur, pompe). Ces capteurs optionnels améliorent le système mais ne sont pas nécessaires.

| Composant | Prix | Utilité | Priorité |
|-----------|------|---------|----------|
| **MQ-7 CO sensor** (🔴 sécurité) | ~3€ | Détecte fuites monoxyde de carbone | **Recommandé** |
| **HA Companion App** (GPS) | 0€ | Compensation altitude automatique | Si altitude > 1000m |
| **Thermocouple K + MAX6675** | ~5€ | T° échappement plus précise | Optionnel |

**Total avec CO sensor: ~18-23€**

### Ce que le fabricant recommande

Le fabricant Autoterm/Planar recommande :
1. **Alimentation stable** : fusible 15A, câbles 6mm² minimum, batterie > 12V au repos
2. **Ne jamais couper l'alimentation** pendant la séquence de purge (5 min après arrêt)
3. **Gardez le panneau d'origine** connecté en parallèle pendant les tests initiaux
4. **Vérifiez les câbles UART** : les fils lâches causent des erreurs CRC et des arrêts intempestifs
5. **Ne pas modifier le firmware** du heater — l'ESP32 est un pont, pas un remplaçant de l'ECU

### Bonnes pratiques de démarrage et d'arrêt

#### Démarrage
1. Vérifier la tension batterie (> 12V) avant de démarrer
2. Laisser le panneau afficher "Standby" avant d'envoyer une commande
3. Ne pas démarrer si T° échappement > 100°C (refroidir d'abord)
4. Le premier démarrage après installation : surveiller les 5 premières minutes dans les logs
5. Activer le mode diagnostic (`diagnostic_mode: true`) les premiers jours pour valider le bon fonctionnement

#### Arrêt
1. **Ne jamais débrancher l'alimentation** pendant la séquence de purge (5 min)
2. La séquence de purge est : `0x0300` (heating) → `0x0304` (cooling) → `0x0305` (idle vent) → `0x0400` (shutdown)
3. Le ventilateur continue de tourner après l'arrêt du brûleur — c'est normal (refroidissement échangeur)
4. L'ESP32 détecte automatiquement la fin de purge et arrête le monitoring
5. En cas d'urgence : appuyer sur "Clear Lockout" dans HA puis redémarrer le ESP32

---

## 🔌 Câblage

### Schéma de connexion

```
┌─────────────┐     ┌──────────────┐     ┌──────────────┐
│   PANNEAU   │     │     ESP32    │     │   CHAUDIÈRE  │
│  (Display)  │     │   DevKit v1  │     │  (Autoterm)  │
├─────────────┤     ├──────────────┤     ├──────────────┤
│      TX  ───┼─────┼── GPIO16 RX  │     │              │
│      RX  ───┼─────┼── GPIO17 TX  │     │              │
│             │     │              │     │              │
│             │     │  GPIO22 RX ──┼─────┼── TX         │
│             │     │  GPIO23 TX ──┼─────┼── RX         │
│             │     │              │     │              │
│   +12V  ────┼──┐  │              │     │   +12V       │
│   GND   ────┼──┼──┼── GND ──────┼─────┼── GND        │
└─────────────┘  │  └──────────────┘     └──────────────┘
                 │
            ┌────┴────┐
            │ 12V→5V  │
            │ Buck    │──── 5V → ESP32 VIN
            └─────────┘
```

### Câblage détaillé

| ESP32 GPIO | Pin | Connecté à | Note |
|------------|-----|------------|------|
| GPIO16 | RX2 | Display TX | Niveau 5V → diviseur ou MAX3232 |
| GPIO17 | TX2 | Display RX | Niveau 3.3V → accepté par le display |
| GPIO22 | RX1 | Heater TX | Niveau 5V → diviseur ou MAX3232 |
| GPIO23 | TX1 | Heater RX | Niveau 3.3V → accepté par la chaudière |
| GND | GND | GND commun | **INDISPENSABLE** |
| VIN | 5V | Buck converter | Alimentation séparée du chauffage |

### ⚠️ ATTENTION NIVEAUX

L'ESP32 utilise du **3.3V**, le protocole Autoterm est en **5V**.

- **TX → RX** (ESP32 vers Autoterm): Le 3.3V est généralement accepté comme HIGH par les récepteurs 5V. **Vérifie** avec un multimètre.
- **RX ← TX** (Autoterm vers ESP32): Le 5V peut **endommager** l'ESP32. **Il faut** un diviseur de tension (2 resistances) ou un MAX3232.

#### Diviseur de tension simple (2 resistances)

```
Autoterm TX (5V) ──[1kΩ]──┬── ESP32 RX (3.3V)
                           │
                        [2kΩ]
                           │
                          GND
```

Rapport: 5V × (2k / (1k+2k)) = 3.33V ✓

---

## ⚡ Alimentation

### Option 1: Buck converter 12V→5V (recommandé)

```
Batterie 12V ──[Fusible 15A]──[Buck converter 12V→5V]── ESP32 VIN
```

- Fil minimum: **6mm²** pour l'alimentation chauffage
- Fusible: **15A** minimum
- Buck converter: capable de fournir **3A+** (le glow pull de l'ESP en consomme ~0.5A au démarrage)

### Option 2: USB (pour les tests uniquement)

- Alimenter l'ESP32 par USB pour les tests
- La chaudière fonctionne normalement avec son alimentation d'origine
- **Ne pas** utiliser l'USB en production (pas assez fiable)

---

## 🔧 Flash ESP32 avec ESPHome

### Prérequis

1. **Home Assistant** installé avec l'intégration ESPHome
2. **ESPHome** addon ou extension installé
3. **ESP32** connecté en USB à votre PC

### Étapes

#### 1. Créer le fichier `secrets.yaml`

Dans le même dossier que `air4d.yaml`:

```yaml
wifi_ssid: "TonWiFi"
wifi_password: "TonMotDePasse"
fallback_password: "AutotermFallback"
web_username: "admin"
web_password: "TonMotDePasse"
```

#### 2. Compiler et flasher via ESPHome Dashboard

1. Ouvrir le dashboard ESPHome (dans Home Assistant ou via `esphome dashboard`)
2. Cliquer sur **+** → **New Device** → **Continue** → **Import project from repository**
3. Ou copier le fichier `air4d.yaml` dans le dossier de configuration ESPHome
4. Cliquer sur **Edit** → vérifier les GPIO
5. Cliquer sur **Install** → **Wirelessly** (si déjà connecté au WiFi) ou **Plug into this computer**

#### 3. Premier démarrage

1. L'ESP32 démarre et crée un point d'accès: `Autoterm-4D Fallback`
2. Se connecter au WiFi `Autoterm-4D Fallback` (mot de passe dans `secrets.yaml`)
3. Ouvrir `192.168.4.1` dans un navigateur
4. Configurer le WiFi principal
5. L'ESP32 redémarre et se connecte au WiFi principal
6. Vérifier dans le dashboard ESPHome que le device est **online**

#### 4. Vérifier les logs

Dans ESPHome Dashboard → cliquer sur **Logs** du device `air4d`:

```
[D][autoterm_uart:XXX] Display connection detected
[I][autoterm_uart:XXX] Startup: 0.0 runtime hours, 0.00L fuel consumed, 0 starts, boot #1
[D][autoterm_uart:XXX] Status: Standby (0x0001) | U=12.4V | Heater 0°C | Fan 0/0 rpm | Pump 0.00 Hz
```

---

## 🏠 Intégration Home Assistant

### 1. Ajouter l'intégration ESPHome

Si ce n'est pas déjà fait:
1. Settings → Devices & Services → **+ Add Integration** → **ESPHome**
2. Entrer l'IP de l'ESP32 (ex: `192.168.1.50`)
3. Entrer la clé API (dans `air4d.yaml` → `api.encryption.key`)

### 2. Entités créées

Automatiquement disponibles dans HA:

| Entité | Type | Description |
|--------|------|-------------|
| `climate.air4d_heating` | Climate | Thermostat complet |
| `sensor.internal_temperature` | Sensor | T° interne |
| `sensor.external_temperature` | Sensor | T° externe |
| `sensor.heater_temperature` | Sensor | T° échappement |
| `sensor.voltage` | Sensor | Tension batterie |
| `sensor.pump_frequency` | Sensor | Fréquence pompe |
| `sensor.fuel_consumption` | Sensor | Conso instantanée |
| `sensor.total_fuel_consumed` | Sensor | Conso totale |
| `sensor.combustion_efficiency` | Sensor | Efficacité |
| `sensor.wear_score` | Sensor | Usure brûleur |
| `sensor.ignition_time` | Sensor | Temps d'allumage |
| `switch.air4d_night_mode` | Switch | Mode nuit |
| `switch.air4d_frost_protection` | Switch | Protection anti-gel |
| `switch.air4d_debug_mode` | Switch | Mode debug |
| `button.air4d_restart_esp` | Button | Redémarrer ESP32 |

### 3. Dashboard recommandé

Dans HA → Settings → Dashboards → **+ Add Dashboard** → créer un dashboard **"Autoterm"**:

```yaml
views:
  - title: 🔥 Autoterm
    path: autoterm
    icon: mdi:fire
    type: sections
    sections:

      # ── Ligne 1: Contrôle principal ──
      - type: grid
        cards:
          - type: thermostat
            entity: climate.air4d_heating
            name: "Chauffage"
          - type: entities
            title: "🔧 Contrôles"
            entities:
              - entity: select.air4d_temperature_source
                name: "Source T°"
              - entity: number.air4d_fan_level
                name: "Niveau ventilateur"
              - entity: switch.air4d_night_mode
                name: "🌙 Mode nuit"
              - entity: switch.air4d_frost_protection
                name: "❄️ Protection anti-gel"
              - entity: switch.air4d_debug_mode
                name: "🐛 Debug mode"

      # ── Ligne 2: Températures ──
      - type: grid
        cards:
          - type: sensor
            entity: sensor.air4d_internal_temp
            name: "T° Interne"
            graph: line
          - type: sensor
            entity: sensor.air4d_external_temp
            name: "T° Externe"
            graph: line
          - type: sensor
            entity: sensor.air4d_heater_temp
            name: "T° Échappement"
            graph: line
          - type: sensor
            entity: sensor.air4d_predicted_temp
            name: "T° Prédite"

      # ── Ligne 3: Performance ──
      - type: grid
        cards:
          - type: gauge
            entity: sensor.air4d_fuel_consumption
            name: "Conso carburant"
            unit: "L/h"
            min: 0
            max: 1
            severity:
              green: 0
              yellow: 0.3
              red: 0.6
          - type: gauge
            entity: sensor.air4d_combustion_efficiency
            name: "Efficacité"
            unit: "%"
            min: 0
            max: 100
            severity:
              green: 60
              yellow: 40
              red: 0
          - type: gauge
            entity: sensor.air4d_voltage
            name: "Tension"
            unit: "V"
            min: 9
            max: 15
            severity:
              green: 12
              yellow: 11
              red: 10
          - type: sensor
            entity: sensor.air4d_pump_frequency
            name: "Pompe"
            unit: "Hz"

      # ── Ligne 4: Eco-Adaptive ──
      - type: grid
        cards:
          - type: gauge
            entity: sensor.air4d_eco_adaptive_level
            name: "Niveau Adaptatif"
            min: 0
            max: 9
            severity:
              green: 3
              yellow: 6
              red: 8
          - type: gauge
            entity: sensor.air4d_eco_power_efficiency
            name: "Efficacité Énergie"
            unit: "%"
            min: 0
            max: 100
            severity:
              green: 60
              yellow: 30
              red: 0
          - type: sensor
            entity: sensor.air4d_eco_adaptive_error
            name: "Erreur T°"
            unit: "°C"
          - type: sensor
            entity: sensor.air4d_eco_mode_status
            name: "État adaptatif"

      # ── Ligne 5: Carburant & Maintenance ──
      - type: grid
        cards:
          - type: statistic
            entity: sensor.air4d_total_fuel_consumed
            name: "Carburant total"
            stat_type: change
            period: total
          - type: statistic
            entity: sensor.air4d_runtime_hours
            name: "Heures totales"
            stat_type: change
            period: total
          - type: sensor
            entity: sensor.air4d_session_runtime
            name: "Durée session"
          - type: sensor
            entity: sensor.air4d_total_starts
            name: "Démarrages"

      # ── Ligne 6: Usure & Maintenance ──
      - type: gauge
        entity: sensor.air4d_wear_score
        name: "Usure brûleur"
        unit: "%"
        min: 0
        max: 100
        severity:
          green: 70
          yellow: 40
          red: 0

      # ── Ligne 7: Diagnostic ──
      - type: entities
        title: "🔬 Diagnostic"
        entities:
          - entity: sensor.air4d_error_code
            name: "Code erreur"
          - entity: sensor.air4d_error_text
            name: "Description erreur"
          - entity: sensor.air4d_firmware_version
            name: "Version firmware"
          - entity: sensor.air4d_error_log
            name: "Journal erreurs"
          - entity: sensor.air4d_boot_count
            name: "Boot count"
          - entity: sensor.air4d_free_heap
            name: "Heap libre"
          - entity: sensor.air4d_reset_reason
            name: "Raison reset"

      # ── Ligne 8: Diagnostic mode (si activé) ──
      - type: entities
        title: "📡 Mode Diagnostic"
        entities:
          - entity: sensor.air4d_glow_plug_current
            name: "Bougie préchauffage"
          - entity: sensor.air4d_chamber_temp
            name: "T° chambre combustion"
          - entity: sensor.air4d_board_temp
            name: "T° carte ECU"

      # ── Ligne 9: Boutons d'action ──
      - type: entities
        title: "⚡ Actions"
        entities:
          - entity: button.air4d_unlock_button
            name: "🔓 Déverrouiller (error 37)"
          - entity: button.air4d_prime_pump_button
            name: "⛽ Amorcer pompe"
          - entity: button.air4d_restart
            name: "🔄 Redémarrer ESP32"
          - entity: button.air4d_safe_mode
            name: "🛡️ Mode sûr"

      # ── Ligne 10: Graphiques temporels ──
      - type: horizontal-stack
        cards:
          - type: history-graph
            title: "📊 T° (24h)"
            entities:
              - entity: sensor.air4d_internal_temp
                name: "Interne"
              - entity: sensor.air4d_external_temp
                name: "Externe"
              - entity: sensor.air4d_heater_temp
                name: "Échappement"
            hours_to_show: 24
          - type: history-graph
            title: "📊 Niveau Adaptatif (24h)"
            entities:
              - entity: sensor.air4d_eco_adaptive_level
                name: "Niveau"
              - entity: sensor.air4d_eco_power_efficiency
                name: "Efficacité %"
            hours_to_show: 24
```

---

## 🧪 Tests

### Test sans panneau

1. Brancher **uniquement** l'ESP32 à la chaudière (GPIO22/23)
2. L'ESP32 détecte l'absence de panneau après 5 secondes
3. Mode autonome: l'ESP32 envoie des requêtes status/settings
4. Commander via HA: `climate.air4d_heating` → `heat` → niveau 4

### Test avec panneau

1. Brancher le panneau à GPIO16/17
2. Brancher l'ESP32 à la chaudière à GPIO22/23
3. L'ESP32 intercepte les trames entre panneau et chaudière
4. Vérifier que le panneau fonctionne normalement
5. Vérifier que les capteurs sont mis à jour dans HA

### Test script Arduino

Pour débugger sans ESPHome:
1. Ouvrir `test_uart.ino` dans Arduino IDE
2. Sélectionner: Board → ESP32 Dev Module
3. Flasher
4. Ouvrir le moniteur série à 115200 baud
5. Appuyer `h` pour voir les commandes disponibles

Commandes disponibles :
- `1` = Power Mode Level 4
- `2` = Power Mode Level 8
- `3` = Fan Only Level 5
- `0` = Standby
- `r` = Request Status
- `v` = Version Firmware
- `u` = Unlock (error 37)
- `p` = Prime Pump 1Hz
- `d` = Diagnostic Mode (72 octets/s)
- `i` = History/Report

---

## 🔍 Dépannage

| Problème | Cause probable | Solution |
|----------|---------------|----------|
| Pas de données capteurs | Mauvais câblage | Vérifier RX/TX (croisés!) |
| Erreurs CRC fréquentes | Bruit UART | Raccourcir câbles, ajouter condensateurs |
| Spannung basse (<11V) | Câbles trop fins | Utiliser 6mm² minimum |
| ESP32 reset en boucle | Watchdog | Vérifier le firmware, regarder les logs |
| Pas de connexion WiFi | Mauvais mot de passe | Utiliser le fallback AP |
| Panneau ne répond plus | ESP32 bloque UART | Redémarrer ESP32 (button Restart) |
| Mode autonomie permanent | Panneau non branché | Vérifier GPIO16/17 |

---

## 📊 Performance attendue

| Métrique | Valeur |
|----------|--------|
| Consommation ESP32 (veille, heater off) | ~50-70mA ⚡ |
| Consommation ESP32 (WiFi actif) | ~140-150mA |
| CPU | 160MHz (auto PM: 40MHz idle) |
| Latence capteurs | 2 secondes |
| Taille firmware | ~450KB |
| Mémoire libre | ~180KB |
| Nombre capteurs | 35+ (25 internes UART + 5 diagnostics + 3 optionnels + 2 HA) |
| Modes de chauffage | 10 (Eco-Adaptive, PID, Hystérésis, Puissance, Ventilation, Nuit, Frost, Economy, Sleep, Autonomous) |
| Commandes protocole | 13 (toutes documentées dans modie.md) |
| protections sécurité | 7 (surtempérature, flameout, sous-tension, burn-out, lockout, watchdog, CO) |

---

## 🔄 Mises à jour

### Via ESPHome Dashboard

1. ESPHome Dashboard → device `air4d` → **Edit**
2. Modifier le YAML si nécessaire
3. **Install** → **Wirelessly**
4. Attendre le OTA (30-60 secondes)

### Via USB

Si le WiFi ne fonctionne plus:
1. Connecter l'ESP32 en USB
2. ESPHome Dashboard → **Install** → **Plug into this computer**

---

## 📝 Notes importantes

1. **Ne jamais débrancher l'ESP32 pendant un flash** — risque de corruption
2. **Toujours garder le panneau connecté** en parallèle pendant les tests
3. **Sauvegarder `secrets.yaml`** — il contient vos mots de passe
4. **Le MQTT est optionnel** — l'ESPHome API suffit pour Home Assistant
5. **Le mode nuit réduit le bruit** — idéal pour la nuit en camping-car
6. **La protection anti-gel** est activée par défaut — désactiver en été
7. **Le dashboard HTML** fonctionne sur téléphone comme sur PC
