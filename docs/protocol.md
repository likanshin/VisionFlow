# VFLW v1 协议

消息头固定 24 字节，使用网络字节序；Python 格式 `!4sHHQII`。

| 偏移 | 字节 | 字段 |
| --- | --- | --- |
| 0 | 4 | ASCII `VFLW` |
| 4 | 2 | version = 1 |
| 6 | 2 | type：1 JPEG 请求、2 JSON 结果、3 JSON 错误 |
| 8 | 8 | request_id，无符号整数 |
| 16 | 4 | payload_length，非零，默认最多 16 MiB |
| 20 | 4 | flags = 0 |

头和体可任意拆分，也可连续发送多个消息。客户端会话内使用唯一请求编号；服务端拒绝尚未完成的重复编号。
客户端不能假定响应顺序，推荐 window <= 配置的 max_inflight_per_client。
服务端响应最多 64 KiB（含头）。JSON `request_id` 与头必须一致。

成功响应示例：

```json
{"request_id":1,"status":"ok","processor":"lane_lines","width":640,"height":360,"edge_pixels":1034,"lines":[[70,359,285,200]],"timings_ms":{"queue":0.1,"decode":0.8,"process":2.1,"prepare":0.1,"total":3.1}}
```

数字仅用于说明字段。`prepare` 是创建结果 JSON 的耗时，不包含最终 JSON dump/网络写入；
`total` 从入队到 JSON 准备完成。`queue` 不包含网络读取及离线等候入队的时间。
端到端耗时由 Python 同一个 monotonic 时钟测量，包含客户端等待 socket 可写。

请求错误：`invalid_image`（无效/超尺寸 JPEG）、`processing_failed`（算法异常）、`dropped`（实时淘汰）。

```json
{"request_id":3,"status":"error","code":"invalid_image","message":"invalid JPEG payload"}
```

不可恢复的错误（magic、version、flags、type、长度、重复未完成 id）记录日志并关闭当前连接。
慢读/慢写连接超时关闭，不保证送达错误 JSON。连接关闭后不重投旧结果，也不自动重试请求。
完整但尚未入队的离线帧不受部分消息读取超时限制。
