# 🔌 Guide de câblage ESP32 ↔ Autoterm Air 2D/4D

> **Attention** : Ce guide est basé sur le reverse engineering du protocole Autoterm. Vérifiez toujours vos branchements avant de connecter. Une mauvaise connexion peut endommager le chauffage ou le ESP32.

---

## Principe

L'ESP32 se place **entre le panneau et le chauffage**, comme un pont (bridge). Il intercepte et transmet toutes les trames.

```
┌──────────┐      ┌──────────┐      ┌──────────┐
│  PANNEAU │◄────►│  ESP32   │◄────►│ CHAUFFAGE│
│ (display)│ UART │ (bridge) │ UART │          │
└──────────┘      └──────────┘      └──────────┘
```

---

## Connecteur Autoterm

Le connecteur sur le chauffage Autoterm est un connecteur **JST-SM 6 broches** (ou similaire selon le modèle). Les broches utiles sont :

| Broche | Fonction | Description |
|--------|----------|-------------|
| 1 | **GND** | Masse commune |
| 2 | **TX** | Données sortantes du chauffage (vers panneau) |
| 3 | **RX** | Données entrantes (du panneau vers chauffage) |
| 4 | **+12V** | Alimentation (ne PAS connecter au ESP32) |

> ⚠️ **ATTENTION** : L'ordre des broches peut varier selon le modèle et l'année. **Vérifiez avec un multimètre** avant de connecter !

---

## Comment identifier les broches

1. **Débranchez le panneau du chauffage**
2. **Branchez le panneau au chauffage** (fonctionnement normal)
3. **Mesurez avec un multimètre** en mode tension DC :
   - La broche qui affiche ~**12V** = VCC (ne pas connecter au ESP32)
   - La broche **GND** = masse (0V)
   - Les deux broches restantes sont TX et RX
4. **Pour identifier TX vs RX** : en mode DEBUG, la broche qui envoie des données (TX du chauffage) fluctue entre 0V et 5V quand le chauffage tourne

---

## Branchements ESP32

### Alimentation

| Autoterm | ESP32 |
|----------|-------|
| GND | **GND** (une broche GND au choix) |
| +12V | **Ne pas connecter** — alimentez le ESP32 séparément (USB ou buck converter 12V→5V) |

### UART Panneau (GPIO16/17)

| Autoterm (côté panneau) | ESP32 |
|--------------------------|-------|
| TX du panneau | **GPIO16** (RX du ESP32) |
| RX du panneau | **GPIO17** (TX du ESP32) |
| GND | **GND** |

### UART Chauffage (GPIO22/23)

| Autoterm (côté chauffage) | ESP32 |
|----------------------------|-------|
| TX du chauffage | **GPIO22** (RX du ESP32) |
| RX du chauffage | **GPIO23** (TX du ESP32) |
| GND | **GND** |

---

## ⚡ CRITIQUE : Conversion de niveau logique

Le protocole Autoterm utilise la logique **5V**. Le ESP32 fonctionne en **3.3V**.

**Sans convertisseur**, le ESP32 peut ne pas lire correctement les signaux 5V, et le chauffage peut ne pas comprendre les signaux 3.3V.

### Solution 1 : Convertisseur de niveau logique (recommandé)

Utilisez un module **MAX3232** ou **bidirectionnel 3.3V↔5V** :

```
Autoterm TX (5V) ──► [CONVERTISSEUR] ──► ESP32 RX (3.3V)
Autoterm RX (5V) ◄── [CONVERTISSEUR] ◄── ESP32 TX (3.3V)
```

### Solution 2 : Diviseur de tension (pour RX du ESP32 uniquement)

Si le ESP32 lit le 5V (côté RX), un diviseur de tension suffit :

```
Autoterm TX (5V) ──[1kΩ]──┬──► ESP32 RX (GPIO16 ou 22)
                           │
                          [2kΩ]
                           │
                          GND
```

La tension arrive à ~3.3V sur le GPIO du ESP32.

### Solution 3 : Sans convertisseur (risqué)

Certains GPIO de l'ESP32 sont **tolérants 5V** (GPIO 12-39 sur ESP32 classique). GPIO16, 17, 22, 23 sont dans cette plage. **Mais** :
- Le ESP32 peut être endommagé à long terme
- Le TX du ESP32 (3.3V) peut ne pas être compris par le chauffage

---

## 📋 Résumé des branchements

```
CHAUFFAGE AUTOTERM                    ESP32 DevKit v1
┌─────────────────┐                   ┌─────────────────┐
│                 │                   │                 │
│  TX ────────────┼───────────────────┼── GPIO16 (RX1)  │  ← Panneau
│  RX ◄───────────┼───────────────────┼── GPIO17 (TX1)  │
│                 │                   │                 │
│  TX ────────────┼───────────────────┼── GPIO22 (RX2)  │  ← Chauffage
│  RX ◄───────────┼───────────────────┼── GPIO23 (TX2)  │
│                 │                   │                 │
│  GND ───────────┼───────────────────┼── GND           │
│                 │                   │                 │
│  +12V           │                   │  ← NE PAS       │
│                 │                   │    CONNECTER     │
└─────────────────┘                   └─────────────────┘
```

---

## Protocole série

| Paramètre | Valeur |
|-----------|--------|
| Débit | **9600 baud** |
| Bits de données | **8** |
| Parité | **Aucune** |
| Bits d'arrêt | **1** |

---

## 🔍 Test rapide sans ESPHome

Avant de compiler ESPHome, testez la communication avec un simple **USB-TTL** branché sur GPIO16/17 du ESP32 :

1. Flashlez un firmware Arduino simple qui lit l'UART et affiche sur USB :

```cpp
void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, 16, 17);  // RX=16, TX=17
}

void loop() {
  if (Serial2.available()) {
    byte b = Serial2.read();
    Serial.printf("%02X ", b);
  }
}
```

2. Si vous voyez des octets `AA 04...` → le câblage UART fonctionne
3. Si rien → vérifiez le câblage et la tension

---

## ⚠️ Précautions

1. **Débranchez toujours le chauffage** avant de modifier les branchements
2. **Ne jamais connecter le +12V** du chauffage au ESP32
3. **Vérifiez les tensions** avec un multimètre avant de connecter le ESP32
4. **Testez d'abord en lecture** (sans envoyer de données) pour vérifier que le chauffage fonctionne normalement avec le ESP32 branché
5. **Gardez le panneau d'origine** branché pendant les tests initiaux

---

## Résolution de problèmes

| Symptôme | Cause possible | Solution |
|----------|---------------|----------|
| Aucune donnée dans les logs | Câblage TX/RX inversé | Inverser TX et RX |
| Aucune donnée dans les logs | Tension trop haute/basse | Vérifier avec multimètre |
| Données corrompues | Mauvais niveau logique | Ajouter convertisseur 3.3V↔5V |
| Le chauffage ne répond pas | TX du ESP32 trop bas (3.3V) | Ajouter convertisseur ou amplifier |
| Erreurs CRC fréquentes | Connexion électrique mauvaise | Vérifier soudures/connexions |
