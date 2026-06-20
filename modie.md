# Protocole Autoterm/Planar — Référence UART

## Paramètres série
- **Baud rate** : 9600
- **Data bits** : 8
- **Parity** : None
- **Stop bits** : 1

## Structure d'une trame

| Octet | Champ | Description |
|-------|-------|-------------|
| 0 | Header | Toujours `0xAA` |
| 1 | Device | `0x03`=Display, `0x04`=Heater, `0x02`=Diagnostic, `0x00`=Boot |
| 2 | Payload Length | Nombre d'octets du payload (0-255) |
| 3 | Reserved | Toujours `0x00` |
| 4 | Command ID | Code de la commande (voir ci-dessous) |
| 5..N | Payload | Données variables |
| N+1, N+2 | CRC-16 | Modbus, little-endian |

## Commandes

| ID (Hex) | Nom | Direction | Payload | Description |
|----------|-----|-----------|---------|-------------|
| `0x01` | **Start** | Ctrl→Heater | 6 bytes | Démarrer chauffage (mode, T°, niveau) |
| `0x02` | **Settings** | Ctrl↔Heater | 6 bytes | Lire/Modifier configuration |
| `0x03` | **Stop** | Ctrl→Heater | 0 byte | Arrêt (envoyer toutes les 20s jusqu'à standby) |
| `0x06` | **Version** | Ctrl↔Heater | 5 bytes | Version firmware (Major.Minor.Patch.Build.Boot) |
| `0x07` | **Diagnostic** | Ctrl→Heater | 1 byte | Activer/Désactiver télémétrie avancée (72 octets/s) |
| `0x08` | **Set Fan** | Ctrl→Heater | 1 byte | Contrôle direct ventilateur (Hz) |
| `0x0B` | **History** | Ctrl↔Heater | 7-9 bytes | Heures totales, démarrages, 3 dernières erreurs |
| `0x0D` | **Unlock** | Ctrl→Heater | 0 byte | Déverrouiller error 37 (lockout) |
| `0x0F` | **Status** | Ctrl↔Heater | 19 bytes | Données capteurs (temp, tension, ventilateur, pompe) |
| `0x11` | **Panel Temp** | Ctrl↔Heater | 1 byte | Température ambiante du panneau |
| `0x13` | **Fuel Pump** | Ctrl→Heater | 1 byte | Amorcer pompe carburant (fréquence Hz) |
| `0x1C` | **Handshake** | Ctrl↔Heater | 0 byte | Initialisation communication |
| `0x23` | **Ventilation** | Ctrl→Heater | 4 bytes | Mode ventilateur seul (niveau 0-9) |

## Codes de status (octets 0-1 de la réponse `0x0F`)

| Code | État |
|------|------|
| `0x0000` | Veille / Arrêt |
| `0x0001` | Standby (en attente de commande) |
| `0x0100` | Purge: refroidissement capteur flamme |
| `0x0101` | Purge: ventilation chambre combustion |
| `0x0200` | Préchauffage en cours |
| `0x0201` | Bougie de préchauffage active |
| `0x0202` | Allumage séquence 1 |
| `0x0203` | Allumage séquence 2 |
| `0x0204` | Montée en puissance (stabilisation) |
| `0x0300` | Chauffage (PID actif) |
| `0x0304` | Refroidissement en cours |
| `0x0305` | Ventilation de repos |
| `0x0323` | Mode ventilateur seul |
| `0x0400` | Arrêt complet |

## Codes d'erreur (octet 2 de la réponse `0x0F`)

| Code | Hex | Description |
|------|-----|-------------|
| 0 | `0x00` | Pas d'erreur |
| 1 | `0x01` | Surchauffe (échappement > 250°C) |
| 2 | `0x02` | Surchauffe potentielle |
| 5 | `0x05` | Défaut capteur flamme |
| 6 | `0x06` | Défaut capteur température |
| 9 | `0x09` | Défaut bougie préchauffage |
| 10 | `0x0A` | Défaut RPM ventilateur |
| 11 | `0x0B` | Défaut capteur air |
| 12 | `0x0C` | Surtension |
| 13 | `0x0D` | Pas de démarrage (2 échecs) |
| 15 | `0x0F` | Sous-tension |
| 16 | `0x10` | Durée ventilation dépassée |
| 17 | `0x11` | Défaut pompe carburant |
| 20 | `0x14` | Pas de communication |
| 29 | `0x1D` | Extinction flamme |
| 30 | `0x1E` | Détection flamme (déjà présente) |
| 31 | `0x1F` | Surchauffe (sortie) |
| 33 | `0x21` | Verrouillage contrôle |
| 37 | `0x25` | **Verrouillé (3 échecs consécutifs)** — nécessite unlock `0x0D` |

## Modes de chauffage (payload de `0x01` et `0x02`)

| Octet | Champ | Valeurs |
|-------|-------|---------|
| 0-1 | Work Time | Minutes (ex: `0x78` = 120 min) |
| 2 | Temp Source | `1`=Interne, `2`=Panel, `3`=Externe, `4`=Puissance |
| 3 | Set Temp | 1-30°C |
| 4 | Wait Mode | `0`=Puissance, `1`=Chauffage+Ventilation, `2`=Chauffage |
| 5 | Power Level | 0-9 |

## Mode Diagnostic (0x07)

Quand activé, le heater envoie un flux de **72 octets** toutes les secondes :

| Offset | Description | Type |
|--------|-------------|------|
| 0-1 | État Major/Minor | uint8 |
| 2-4 | Temps cycle total | uint24 BE |
| 11-12 | Ventilateur cible/actuel | uint8 (Hz) |
| 13-16 | Bougie préchauffage cible/actuel | uint16 BE (PWM) |
| 17 | Pompe carburant | uint8 (/ 100 = Hz) |
| 18-19 | T° chambre combustion | uint16 BE (Kelvin) |
| 20-21 | T° flamme | uint16 BE (Kelvin) |
| 24 | T° externe | int8 (°C) |
| 25 | T° carte ECU | int8 (°C) |
| 26-27 | Tension | uint16 BE (/ 10 = V) |
| 28 | Code défaut | uint8 |
