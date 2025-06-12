import serial
import struct
import pyaudio
import threading
import queue
import time

# Serial config
ser = serial.Serial(
    port='COM13',
    baudrate=921600,
    parity=serial.PARITY_NONE,
    stopbits=serial.STOPBITS_ONE,
    bytesize=serial.EIGHTBITS,
    timeout=2
)

marker = b'\xDE\xAD\xBE\xEF'
FRAME_SIZE = 3300#2750  # 1375 samples x 2 bytes
SAMPLES = 1650#1375

# Audio settings
RATE = 22050
CHANNELS = 1
FORMAT = pyaudio.paInt16

# Queue for data between threads
audio_queue = queue.Queue(maxsize=10)

# PyAudio setup
p = pyaudio.PyAudio()
stream = p.open(format=FORMAT,
                channels=CHANNELS,
                rate=RATE,
                output=True)

def wait_for_marker():
    buffer = b''
    while True:
        buffer += ser.read(1)
        if len(buffer) > len(marker):
            buffer = buffer[-len(marker):]
        if buffer == marker:
            return

# 🎙️ 接收音訊 thread
def receive_audio():
    print("📡 開始接收 serial 音訊...")
    try:
        while True:
            # print("in recieve while")
            if ser.read(4) != marker:
                wait_for_marker()

            data = ser.read(FRAME_SIZE)
            if len(data) != FRAME_SIZE:
                continue

            samples = struct.unpack('<1650h', data)#'<1375h', data)

            try:
                audio_queue.put(samples, timeout=1)
            except queue.Full:
                print("⚠️ Queue 滿了，資料丟失")
    except Exception as e:
        print("❌ 接收錯誤：", e)

# 🔊 播放 thread
def play_audio():
    print("🎧 開始播放音訊...")
    try:
        while True:
            try:
                samples = audio_queue.get(timeout=1)
                stream.write(struct.pack(f"{len(samples)}h", *samples))
            except queue.Empty:
                print("🔇 等待資料中...")
    except Exception as e:
        print("❌ 播放錯誤：", e)

# ▶️ 啟動雙執行緒
t1 = threading.Thread(target=receive_audio, daemon=True)
t2 = threading.Thread(target=play_audio, daemon=True)

t1.start()
t2.start()

print("🎧 Real-time 音訊播放中，按 Ctrl+C 結束")

try:
    while True:
        time.sleep(1)
except KeyboardInterrupt:
    print("\n🛑 中止中...")
    stream.stop_stream()
    stream.close()
    p.terminate()
    ser.close()
