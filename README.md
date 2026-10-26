# 🐠 Anakino Aquarium ESPUI v3

Controlador para acuario basado en **ESP32** con panel web moderno, control de relés, temporizadores automáticos, sensor de temperatura y actualización OTA (por WiFi).

Reescritura completa del proyecto original de [Atorcha](https://github.com/Atorcha/Anakino_Aquarium_ESPUI), modernizada a un único archivo `.ino` con arquitectura interna por clases, bucle no bloqueante y API JSON.

---

## ✨ Características

- 🌡️ **Temperatura**: lectura DS18B20 con control por histeresis del calentador (modo seguro: si el sensor falla, el calentador se apaga).
- 🔌 **4 Relés**: Luz, Aireador, Calentador y Extra — cada uno con modo Manual o Automático.
- ⏰ **Temporizadores**: horarios por relé con soporte para cruces de medianoche (hora sincronizada por NTP).
- 💻 **Panel Web**: interfaz responsive oscura, login con contraseña hasheada (SHA-256) y token de sesión.
- 💾 **Persistencia NVS**: todos los parámetros (WiFi, usuario, temperatura objetivo, histeresis) sobreviven a reinicios.
- 📶 **WiFi inteligente**: crea una red de configuración (`Anakino_Config`) si no hay WiFi guardada; después se conecta a tu red.
- 🔄 **OTA**: actualiza el firmware por WiFi desde el Arduino IDE sin cables.

---

## 🧩 Componentes necesarios

| Componente | Descripción |
| :--- | :--- |
| ESP32 | Placa de expansión con USB-C (CH340C) |
| DS18B20 | Sensor de temperatura sumergible |
| 4x Relés | Sólidos o electromecánicos (GPIO control) |

---

## 🛠️ Instalación del software

### 1. Librerías (Arduino IDE → Gestor de librerías)

- `ArduinoJson` — Benoit Blanchon (v7+)
- `OneWire` — Paul Stoffregen
- `DallasTemperature` — Miles Burton

### 2. Núcleo ESP32

En el gestor de placas, instala **esp32 by Espressif Systems** (versión 2.0.x o superior). Selecciona la placa **ESP32 Dev Module**.

### 3. Compilar y subir

1. Abre `Anakino_Aquarium_ESPUI.ino`.
2. Revisa la sección **CONFIGURACIÓN DE PINES** al principio del código y ajusta los GPIO a tu cableado.
3. Sube por USB.

---

## 🔧 Primer arranque

1. Al encender sin WiFi configurada, el ESP32 crea la red **`Anakino_Config`**.
2. Conéctate a ella y abre `http://192.168.4.1`.
3. Introduce SSID y contraseña de tu WiFi.
4. El dispositivo se reiniciará y conectará a tu red. La IP aparece en el monitor serie (115200 baudios).
5. Abre esa IP en cualquier navegador.

> 💡 Recomendado: asigna una **IP fija** al ESP32 desde tu router.

---

## 🔐 Acceso al panel

| Usuario | Contraseña |
| :--- | :--- |
| `Anakin` | `1234` |

⚠️ Cambia estas credenciales desde el panel en cuanto entres. La contraseña se guarda **hasheada** en la memoria flash (nunca en texto plano).

---

## 🎛️ Uso del panel

- **Interruptores**: encienden/apagan manualmente (cambia a modo Manual).
- **Horarios**: define hora de inicio; si el relé estaba manual, pasa a Automático.
- **Ajustes**: temperatura objetivo, histeresis, activar/desactivar calentador, cambiar usuario y contraseña.
- **UPDATE (OTA)**: deja el modo OTA activo. Para subir nuevo firmware, selecciona el puerto de red `anakino` en el Arduino IDE y sube normalmente.

---

## ⚙️ Configuración avanzada

Edita las constantes al inicio del `.ino`:

```cpp
#define PIN_RELAY_LUZ       25
#define PIN_RELAY_AIREADOR  26
#define PIN_RELAY_CALENT    27
#define PIN_RELAY_EXTRA     14
#define RELAY_ACTIVE_LOW    true   // false si tus relés se activan con HIGH
#define PIN_DS18B20         4
