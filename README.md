# NH-03 BIT · Asistente IA estilo TRON con ESP32-S3 y Gemini

**Bit** es un asistente de escritorio inspirado en el Bit de TRON. Le preguntas lo que quieras por voz y solo contesta **SÍ**, **NO** o **NO LO SÉ**, cada respuesta con su propia animación 3D y su sonido. También tiene un modo de respuesta en texto y una pantalla del tiempo, y muestra la hora y la señal WiFi.

Funciona solo: sin PC ni servidor, conectado por WiFi a la API gratuita de Gemini.

📺 Vídeo: https://youtu.be/pNFVUuRyfzM
🖨️ Carcasa (STL): https://www.printables.com/model/1907942-nh-03-bit-carcasa-low-poly-para-asistente-ia-esp32

Proyecto de **NANOHUMANO LABS**.

---

## 🔧 Hardware

- Placa **Diymore / LCDWIKI ES3C28P**: ESP32-S3 con pantalla táctil capacitiva de 2,8" (FT6336G), micrófono, códec ES8311 y LED RGB.
  ⚠️ La **ES3N28P** parece igual pero **no es táctil**. Elige siempre la **ES3C28P**.
- Altavoz de 40 × 30 × 10 mm (conector de altavoz de la placa).
- Carcasa low-poly impresa en 3D (enlace arriba).
- Cable USB-C y cargador o power bank.

💡 Aliméntalo con cargador o power bank. Conectado al USB del PC, el micrófono puede coger ruido.

## 💻 Software

**Arduino IDE** con el core **ESP32 3.x**.

Placa: `ESP32S3 Dev Module`
- PSRAM: `OPI PSRAM`
- Flash Size: `16MB`

Librerías:
- [LovyanGFX](https://github.com/lovyan03/LovyanGFX)
- [WiFiManager](https://github.com/tzapu/WiFiManager) (tzapu)
- [ArduinoJson](https://arduinojson.org/) v7

Descarga el repositorio (**Code → Download ZIP**) y pon los tres archivos (`NH03_Bit.ino`, `Bit3D.h` y `BitLogic.h`) juntos en una carpeta llamada **`NH03_Bit`**. Abre `NH03_Bit.ino` y súbelo a la placa.

## ⚙️ Configuración

No hace falta tocar el código para poner el WiFi ni la clave:

1. Consigue una API key gratuita de Gemini en [Google AI Studio](https://aistudio.google.com/apikey).
2. Al encender Bit por primera vez, crea la red WiFi **`Bit-Setup`**. Conéctate con el móvil y se abrirá el portal.
3. Elige tu WiFi y pega tu **clave de Gemini**. El modelo se puede cambiar ahí también.
4. Para volver al portal más tarde, **mantén pulsado 1,5 s el icono WiFi** (arriba a la derecha).

La clave se guarda en la memoria de la placa y no aparece en el código.

**Ubicación del tiempo:** por defecto es Andratx (Mallorca). Cambia `LAT_S` y `LON_S` al principio del `.ino` por las coordenadas de tu ciudad. El tiempo lo da [Open-Meteo](https://open-meteo.com/), sin clave.

## 🕹️ Uso

- **Tocar la pantalla** (modo BIT o TEXTO): habla tu pregunta.
- **Deslizar a izquierda o derecha**: cambia entre BIT, TEXTO y TIEMPO.
- **Monitor serie (115200)**: escribe una frase y se envía como pregunta. Comandos disponibles: `KEY <clave>`, `MODEL <modelo>`, `STATUS`, `ANS SI|NO|NOSE`, `TXT <texto>`, `SND <n>`, `W`, `VOL+`, `VOL-`.

Tiene un límite propio de 150 preguntas al día para no pasarse de la cuota gratuita.

## 📁 Archivos

| Archivo | Qué hace |
|---|---|
| `NH03_Bit.ino` | Programa principal: pantalla, audio, WiFi, Gemini y tiempo |
| `Bit3D.h` | Motor 3D mínimo para dibujar a Bit (poliedros con sombreado plano) |
| `BitLogic.h` | Lógica de texto pura, sin Arduino (se puede probar en el PC) |

---

¿Te atascas montándolo o tienes una idea para mejorar a Bit? Déjalo en los comentarios del vídeo o abre un *issue*.
