# Phase 0 環境確認 Checklist — Claire

> Ray 板已完成相同流程。請逐項確認並把結果填入括號。

---

## 1. 板子開機確認

- [ ] JP4 跳線撥到 SD 位置(板子上有標示)
- [ ] Micro-USB 接電腦或充電器供電
- [ ] 乙太網路線接到電腦或路由器

---

## 2. SSH 連線到板子

板子開機後,在電腦上開 PowerShell 或 CMD,輸入:

```
ssh xilinx@192.168.2.99
```

出現提示後輸入密碼:
```
xilinx
```

看到 `xilinx@pynq:~$` 代表連線成功。

**常見問題:**
- 若出現 `WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED`:在 PowerShell 執行 `ssh-keygen -R <板子IP>` 清除舊紀錄再重連。
- 若連不上:確認電腦與板子在同一網段,或改用直連(電腦固定 IP 設同網段)。

- [ ] SSH 連線成功,看到 `xilinx@pynq:~$`

---

## 3. PYNQ image 版本確認

SSH 連線後執行:

```bash
cat /etc/os-release
```

- [ ] 版本為 `PynqLinux 2.5 (Glasgow)`,基於 Ubuntu 18.04
- 實際版本:__________

> 若版本不同,和 Ray 對齊後再繼續。

---

## 4. base overlay 確認

```bash
ls /home/xilinx/pynq/overlays/base/
```

- [ ] 有 `base.bit` 和 `base.hwh`

---

## 5. audio bypass 測試(若手邊沒有耳機或音源可暫緩)

瀏覽器開Jupyter。

新建一個 Python 3 notebook,貼上以下程式碼並執行:

```python
from pynq.overlays.base import BaseOverlay

ol = BaseOverlay("base.bit")
audio = ol.audio
audio.configure(sample_rate=48000)
audio.select_line_in()
audio.bypass(seconds=15)
```

接線:
- 任何音源(eg. 麥克風) → 3.5mm音源線 → 板子 LINE IN
- 板子 HP OUT → 3.5mm音源線 → 耳機或喇叭

- [ ] 執行不報錯
- [ ] HP OUT 有聲音輸出

---

## 6. Git 環境與 repo clone

### 6.1 確認 Git 已安裝

在電腦上開 PowerShell:

```
git --version
```

若出現版本號(如 `git version 2.x.x`)代表已安裝。若顯示找不到指令,請至 https://git-scm.com 下載安裝。

- [ ] Git 已安裝

### 6.2 Clone repo

選一個你想放專案的資料夾(例如桌面),在 PowerShell 執行:

```
cd ~/Desktop
git clone https://github.com/RayGur/bass-fx-on-pynq.git
```

完成後進入資料夾:

```
cd bass-fx-on-pynq
```

### 6.3 確認最新狀態

```
git log --oneline -5
```

應該看到最近幾筆 commit 紀錄。

- [ ] Clone 成功
- [ ] 看得到最新 commit

---
