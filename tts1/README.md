# ESP32-C5 Doubao TTS Streaming Demo

这是一个基于 ESP-IDF 的 ESP32-C5 示例工程，当前只保留本次语音链路相关代码：

- WiFi 自动扫描并连接已知热点
- I2S Speaker PCM16 单声道播放
- Doubao-TTS 2.0 HTTPS POST 流式请求
- MP3 chunk/SSE/JSON 流解析
- minimp3 解码
- ringbuffer + FreeRTOS task 边下载边播放

## 需要本地填写的配置

### WiFi

编辑：

```text
components/BSP/wifi/wifi_credentials.h
```

把模板替换成你自己的 WiFi：

```c
static const known_wifi_t WIFI_KNOWN_LIST[] = {
    {"YOUR_WIFI_SSID", "YOUR_WIFI_PASSWORD"},
};
```

### Doubao-TTS API Key

编辑：

```text
components/BSP/doubao_tts/doubao_tts.h
```

填写：

```c
#define DOUBAO_TTS_API_KEY "YOUR_X_API_KEY"
```

默认资源 ID 已设置为：

```c
#define DOUBAO_TTS_RESOURCE_ID "seed-tts-2.0"
```

默认音色：

```c
#define DOUBAO_TTS_VOICE_TYPE "zh_female_vv_uranus_bigtts"
```

### 测试文本

编辑：

```text
main/main_config.h
```

修改：

```c
#define MAIN_TTS_TEST_TEXT "你好，ESP32-C5"
```

## 目录说明

```text
components/BSP/IIS/          I2S Speaker 播放
components/BSP/wifi/         WiFi 管理和账号模板
components/BSP/doubao_tts/   Doubao-TTS 2.0 + MP3 解码流式播放
main/                        app_main 和测试入口
```

## 安全说明

仓库中不保存真实 WiFi 密码和真实 Doubao API Key。

上传 GitHub 前请保持这些值为空或模板值。
