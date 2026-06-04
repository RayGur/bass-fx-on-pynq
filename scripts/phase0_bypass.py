from pynq.overlays.base import BaseOverlay

ol = BaseOverlay("base.bit")
audio = ol.audio
audio.configure(sample_rate=48000)
audio.select_line_in()
audio.bypass(seconds=30)
