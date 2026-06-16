# Bass FX Demo 環境設定 — Claire 的 Windows 筆電

> PYNQ-Z2 即時 Bass 效果器 demo 指南。涵蓋兩種操作模式：
> - **Mode A**：SSH 直接在板上啟動效果器（板子 + 喇叭即可 demo）
> - **Mode B**：PC 端 GUI 遠端控制（需額外設定 Python 環境）

---

## 一、必要硬體

| 項目 | 說明 |
|------|------|
| PYNQ-Z2 板子 | Ray 負責燒錄 bitstream |
| Ethernet 線 | 筆電直連板子（或同一路由器） |
| 3.5mm 音源線 ×2 | bass → LINE IN、HP OUT → 喇叭/耳機 |
| 電源線 | Micro-USB 供電 |

---

## 二、網路設定（一次性）

板子 IP 固定為 `192.168.2.99`，需把筆電有線網卡設同網段：

1. 搜尋「**檢視網路連線**」→ 找到「乙太網路」→ 右鍵 → **內容**
2. 選「**網際網路通訊協定第 4 版 (TCP/IPv4)**」→ **內容**
3. 選「使用下列的 IP 位址」：
   - IP 位址：`192.168.2.100`
   - 子網路遮罩：`255.255.255.0`
   - 預設閘道：（留空）
4. 確定

驗證連通：

```powershell
ping 192.168.2.99
```

收到回應即成功。

---

## 三、SSH 連線到板子

開啟 PowerShell：

```powershell
ssh xilinx@192.168.2.99
```

密碼：`xilinx`

看到 `xilinx@pynq:~$` 即為成功。

> **首次連線**可能出現 fingerprint 確認，輸入 `yes` 繼續。
>
> 若出現 `WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED`：
> ```powershell
> ssh-keygen -R 192.168.2.99
> ```
> 再重新 SSH。

---

## Mode A：SSH 直接在板上執行效果器

### 啟動效果器

SSH 登入後執行：

```bash
bash ~/bass-fx/ui_dev/start.sh
```

正常輸出：

```
[start] Initialising codec...
[start] Codec ready. Starting audio DMA loop...
```

看到第二行後效果器開始運作，撥弦即可聽到聲音通過。

效果器會持續執行直到手動停止（`Ctrl+C`）。

### 即時調整參數

另開一個 PowerShell 視窗，再次 SSH 連線：

```powershell
ssh xilinx@192.168.2.99
```

執行控制程式：

```bash
sudo python3 ~/bass-fx/ui_dev/ctrl_client.py
```

輸入密碼 `xilinx` 後，可下即時命令（每行一條）：

```
dist_en 1          # 開啟 distortion
dist_en 0          # 關閉 distortion
wobble_en 1        # 開啟 wobble
wobble_en 0        # 關閉 wobble
threshold 0.20     # distortion threshold（0.05–0.90）
gain 12            # distortion gain（1–20）
lfo_rate 2.0       # wobble 速度 Hz（0.5–8.0）
lfo_depth 80       # wobble 深度（0–100）
lfo_floor 6        # wah depth preset（0=A, 4=B, 6=C）
```

同時 stdout 每 100ms 印出板子狀態：

```
STATE sw=01 dist_preset=0 wobble_preset=1 wah=0
```

按 `Ctrl+C` 離開控制程式。

### 物理控制（板子上的按鈕）

不需開電腦也可控制：

| 硬體 | 功能 |
|------|------|
| sw[0]（最右邊撥桿） | 開/關 distortion |
| sw[1] | 開/關 wobble |
| btn[0] | distortion preset 切換（L↔H） |
| btn[1] | wobble speed preset 切換（SLOW↔FAST） |
| btn[2] | wah depth preset 循環（A→B→C→A） |
| LD4/LD5（RGB LED） | 顯示 sw 狀態 |
| led[0]/led[1] | 顯示 preset 強度 |

### 停止效果器

在執行 `start.sh` 的視窗按 `Ctrl+C`。

---

## Mode B：PC 端 GUI 遠端控制

### B1. 安裝 Python 3

1. 至 [https://www.python.org/downloads/](https://www.python.org/downloads/) 下載最新 Python 3
2. 安裝時**務必勾選「Add Python to PATH」**
3. 安裝完成後重開 PowerShell，確認：

```powershell
python --version
```

顯示 `Python 3.x.x` 即成功。

### B2. 安裝 Git（若尚未安裝）

```powershell
git --version
```

若找不到指令，至 [https://git-scm.com/download/win](https://git-scm.com/download/win) 下載安裝，重開 PowerShell。

### B3. Clone Repo

```powershell
cd ~/Desktop
git clone https://github.com/RayGur/bass-fx-on-pynq.git
cd bass-fx-on-pynq
git checkout feat/ui
```

### B4. 安裝 paramiko

```powershell
pip install paramiko
```

確認：

```powershell
python -c "import paramiko; print('paramiko OK')"
```

### B5. 啟動 GUI

在 `bass-fx-on-pynq` 資料夾下：

```powershell
python ui\bass_ui.py
```

### B6. GUI 操作流程

1. 填入連線資訊：
   - Host：`192.168.2.99`
   - User：`xilinx`
   - Pass：`xilinx`
2. 按 **Connect** → 等待 `● Connected`（綠色）
3. 按 **▶ Start FX** → 等待約 5–10 秒 → `● Running`
4. 開始操作效果器

**GUI 控制項說明：**

| 控制項 | 功能 |
|--------|------|
| STOMP 踏板（大圓鈕） | 點擊切換效果開/關；與板子 sw 雙向同步 |
| THRESH / GAIN 旋鈕 | 上下拖曳調整，即時送出參數 |
| RATE / DEPTH 旋鈕 | 上下拖曳調整 wobble 速度/深度 |
| PRESET L / H | distortion 低/高強度 preset |
| WAH A / B / C | wah depth preset，與板子 btn[2] 雙向同步 |
| SPEED SLOW / FAST | wobble 速度 preset，與板子 btn[0/1] 雙向同步 |
| SW:XY（右上角） | 即時顯示板上物理撥桿狀態 |

**停止：** 按 **■ Stop** → `● Stopped` 後可再次 **▶ Start FX**，不需重連。

---

## 常見問題

| 問題 | 解法 |
|------|------|
| `ping 192.168.2.99` 沒回應 | 確認 Ethernet 線有插；確認筆電有線網卡 IP 設為 `192.168.2.100` |
| SSH 連不上 | 先 ping 通再試；確認板子有開機（LED 有亮） |
| `python` 找不到指令 | 重裝 Python，安裝時勾 Add to PATH，重開 PowerShell |
| `git` 找不到指令 | 安裝 Git for Windows，重開 PowerShell |
| `audio_dma` 執行失敗 | SSH 上板，確認 `~/bass-fx/ui_dev/audio_dma` 存在；若缺少找 Ray |
| GUI `● Error: Authentication failed` | User `xilinx`，Pass `xilinx` |
| GUI Start FX 一直轉 | 先用 Mode A 確認板子可正常啟動 |
| 音訊有茲擦聲 | 已知限制（類比端 EMI），不影響 demo |

---

## 快速 Checklist

### Mode A
- [ ] `ping 192.168.2.99` 有回應
- [ ] SSH 連線成功（`xilinx@pynq:~$`）
- [ ] `bash ~/bass-fx/ui_dev/start.sh` 看到 `Codec ready`
- [ ] 撥弦有聲音通過
- [ ] sw[0]/sw[1] 撥桿可切換 distortion / wobble

### Mode B（在 Mode A 基礎上）
- [ ] `python --version` 顯示 Python 3.x
- [ ] `python -c "import paramiko; print('OK')"` 顯示 OK
- [ ] `git clone` 成功，切換到 `feat/ui` branch
- [ ] `python ui\bass_ui.py` 開啟 GUI
- [ ] Connect → Start FX → `● Running`
- [ ] GUI STOMP 踏板與板子 sw 雙向同步正常
