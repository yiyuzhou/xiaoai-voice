# 第三方来源与许可
根目录 MIT 只适用于本项目代码；第三方许可继续适用，发布安装包时应随包保留各依赖许可证。

| 组件 | 许可与来源 |
| --- | --- |
| rolazd/xiaoai-doubao | [MIT](https://github.com/rolazd/xiaoai-doubao/blob/main/LICENSE)，xiaoai-voice的上游项目，版权声明见根目录 `LICENSE` |
| kslr/xiaoai-plus | [MIT](https://github.com/kslr/xiaoai-plus/blob/main/LICENSE)，`xiaoai-plus/LICENSE` 原样保留 |
| idootop/open-xiaoai | [MIT](https://github.com/idootop/open-xiaoai/blob/main/LICENSE)，研究参考 |
| sherpa-onnx | [Apache-2.0](https://github.com/k2-fsa/sherpa-onnx/blob/master/LICENSE)，v1.12.25，构建时下载 |
| KWS 权重、tokens、en.phone | [官方模型下载](https://k2-fsa.github.io/sherpa/onnx/kws/pretrained_models/index.html)；权重独立再分发许可未确认，不入仓库，使用者自行下载并核实许可；不能把代码 Apache 许可直接等同于权重许可 |
| nlohmann/json | [MIT](https://github.com/nlohmann/json/blob/develop/LICENSE.MIT)，v3.11.3 |
| IXWebSocket | [BSD-3-Clause](https://github.com/machinezone/IXWebSocket/blob/master/LICENSE.txt)，v11.4.5 |
| spdlog | [MIT](https://github.com/gabime/spdlog/blob/v1.x/LICENSE)，v1.15.1；内含 fmt 的 MIT 许可 |
| simple-sentencepiece / OpenFst / kaldi-native-fbank | [Apache-2.0](https://github.com/pkufool/simple-sentencepiece/blob/master/LICENSE) / [Apache-2.0](https://www.openfst.org/) / [Apache-2.0](https://github.com/csukuangfj/kaldi-native-fbank/blob/master/LICENSE) |
| kissfft | [BSD-3-Clause](https://github.com/mborgerding/kissfft/blob/master/COPYING) |
| ONNX Runtime | [MIT](https://github.com/microsoft/onnxruntime/blob/main/LICENSE)，sherpa 构建依赖 |
| WebRTC audio processing | [BSD-3-Clause](https://freedesktop.org/software/pulseaudio/webrtc-audio-processing/)，构建依赖 |
| OpenSSL / zlib | [Apache-2.0](https://www.openssl.org/source/license.html) / [zlib](https://zlib.net/zlib_license.html) |
| ca-certificates | [curl Mozilla CA bundle](https://curl.se/docs/caextract.html)，提取来源 Mozilla（MPL-2.0）；证书本身按各自条款，不随源码分发 |
| FastAPI | [MIT](https://github.com/fastapi/fastapi/blob/master/LICENSE) |
| Uvicorn | [BSD-3-Clause](https://github.com/encode/uvicorn/blob/main/LICENSE.md) |
| cryptography | [Apache-2.0 OR BSD-3-Clause](https://github.com/pyca/cryptography/blob/main/LICENSE) |
| Pydantic / Starlette | [MIT](https://github.com/pydantic/pydantic/blob/main/LICENSE) / [BSD-3-Clause](https://github.com/encode/starlette/blob/main/LICENSE.md)，FastAPI 依赖 |
| HTTPX / pytest | [BSD-3-Clause](https://github.com/encode/httpx/blob/master/LICENSE.md) / [MIT](https://github.com/pytest-dev/pytest/blob/main/LICENSE)，仅测试 |

按对应机型的安装流程下载，不随项目包再分发：
- [官方KWS模型归档（2025-12-20）](https://github.com/k2-fsa/sherpa-onnx/releases/download/kws-models/sherpa-onnx-kws-zipformer-zh-en-3M-2025-12-20.tar.bz2)，chunk-8；encoder与joiner取int8、decoder取浮点版本，与作者装机文件一致；tokens.txt、en.phone从同一归档提取。
- [固定日期的Mozilla CA bundle](https://curl.se/ca/cacert-2026-11-05.pem)。下载SHA-256固定在setup/speaker.py。
- [小米OH2P 1.62.2 OTA](https://cdn.cnbj1.fds.api.mi-img.com/xiaoqiang/rom/oh2p/mico_all_616cd9d93_1.62.2.bin)，仅在.local处理，不分发固件。
- [Khadas update固定提交](https://github.com/khadas/utils/tree/0dee958a4c3c53c0546ef98758ed721183f11a5f/aml-flash-tool/tools/macos)，仅运行时下载；预编译工具不等于源码已审计。
- [上游OTA解析器](https://github.com/stevenjoezhang/xiaoai-agent/blob/352661832dc609da15839cfde82ea6982f91787e/deploy/client-patch/src/extract.py)，MIT（原头部注明csftech）；固定提交及哈希，运行时下载，不收源码。
