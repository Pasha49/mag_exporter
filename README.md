
# Mag250/Mag254 exporter for Prometheus

Exporter for Prometheus for MAG250/254 set-top boxes. Used for monitoring during continuous STB operation when converting UDP/HTTP streams into analog output. Dashboard for Grafana is present

## Build
Prepare image: ```sudo docker build -t mag-c-builder .```
Build:
```./build.sh```
or
```bash
sudo docker run --rm -v "$PWD":/usr/src/app mag-c-builder sh4-linux-gcc -O3 -static main.c -o mag_exporter && \
    sudo docker run --rm -v "$PWD":/usr/src/app mag-c-builder sh4-linux-strip mag_exporter
```
## Metrics
```json
mag250_device_info{version="1.31",fw_desc="Firmware_name_mag250_218r18.1",mac="00:1a:79:11:22:33",codec="mpeg2",udp_addr="239.0.2.104"} 1
node_procs_total 97
node_temperature_celsius 75.00
node_uptime_seconds 512425.79
mag_exporter_last_update_timestamp 947197225
node_load1 20.80
node_load5 20.80
node_load15 20.81
node_cpu_seconds_total{mode="user"} 10294.68
node_cpu_seconds_total{mode="system"} 68600.74
node_cpu_seconds_total{mode="idle"} 423435.50
node_cpu_seconds_total{mode="iowait"} 0.00
node_memory_MemTotal_bytes 143814656
node_memory_MemFree_bytes 94695424
node_memory_Buffers_bytes 0
node_memory_Cached_bytes 17358848
node_network_receive_bytes_total 3016804109
node_network_receive_errors_total 0
node_network_transmit_bytes_total 139089004
mag250_udp_streams_total 1
mag250_process_cpu_ticks{name="PlayerApp",mode="user"} 974940
mag250_process_cpu_ticks{name="PlayerApp",mode="system"} 2075478
mag250_process_blocked{name="PlayerApp"} 0
mag250_process_cpu_ticks{name="AUD[0].PESTask",mode="user"} 0
mag250_process_cpu_ticks{name="AUD[0].PESTask",mode="system"} 449925
mag250_process_blocked{name="AUD[0].PESTask"} 0
mag250_process_cpu_ticks{name="AUD[0].DecTask",mode="user"} 0
mag250_process_cpu_ticks{name="AUD[0].DecTask",mode="system"} 162626
mag250_process_blocked{name="AUD[0].DecTask"} 0
mag250_process_cpu_ticks{name="AUD[0].PPTask",mode="user"} 0
mag250_process_cpu_ticks{name="AUD[0].PPTask",mode="system"} 152050
mag250_process_blocked{name="AUD[0].PPTask"} 0
mag250_process_cpu_ticks{name="AUD[1].PPTask",mode="user"} 0
mag250_process_cpu_ticks{name="AUD[1].PPTask",mode="system"} 138934
mag250_process_blocked{name="AUD[1].PPTask"} 0
mag250_process_cpu_ticks{name="AUD[2].PPTask",mode="user"} 0
mag250_process_cpu_ticks{name="AUD[2].PPTask",mode="system"} 137469
mag250_process_blocked{name="AUD[2].PPTask"} 0
mag250_process_cpu_ticks{name="AUD[0].EncTask",mode="user"} 0
mag250_process_cpu_ticks{name="AUD[0].EncTask",mode="system"} 0
mag250_process_blocked{name="AUD[0].EncTask"} 0
mag250_process_cpu_ticks{name="AUD[0].PCMPTask",mode="user"} 0
mag250_process_cpu_ticks{name="AUD[0].PCMPTask",mode="system"} 9
mag250_process_blocked{name="AUD[0].PCMPTask"} 0
mag250_process_cpu_ticks{name="AUD[1].PCMPTask",mode="user"} 0
mag250_process_cpu_ticks{name="AUD[1].PCMPTask",mode="system"} 32258
mag250_process_blocked{name="AUD[1].PCMPTask"} 0
mag250_process_cpu_ticks{name="AUD[0].SPDIFP",mode="user"} 0
mag250_process_cpu_ticks{name="AUD[0].SPDIFP",mode="system"} 31968
mag250_process_blocked{name="AUD[0].SPDIFP"} 0
mag250_process_cpu_ticks{name="STVID.InjecterT",mode="user"} 0
mag250_process_cpu_ticks{name="STVID.InjecterT",mode="system"} 30850
mag250_process_blocked{name="STVID.InjecterT"} 1
mag250_process_cpu_ticks{name="STVID[0].MPEG2P",mode="user"} 0
mag250_process_cpu_ticks{name="STVID[0].MPEG2P",mode="system"} 204221
mag250_process_blocked{name="STVID[0].MPEG2P"} 0
mag250_process_cpu_ticks{name="STVID[0].Produc",mode="user"} 0
mag250_process_cpu_ticks{name="STVID[0].Produc",mode="system"} 1008448
mag250_process_blocked{name="STVID[0].Produc"} 1
mag250_process_cpu_ticks{name="STVID[0].Displa",mode="user"} 0
mag250_process_cpu_ticks{name="STVID[0].Displa",mode="system"} 711604
mag250_process_blocked{name="STVID[0].Displa"} 0
mag250_process_cpu_ticks{name="STVID[0].ErrorR",mode="user"} 0
mag250_process_cpu_ticks{name="STVID[0].ErrorR",mode="system"} 15
mag250_process_blocked{name="STVID[0].ErrorR"} 0
mag250_hw_decoder_active{type="video"} 1
mag250_hw_decoder_active{type="audio"} 1
```

