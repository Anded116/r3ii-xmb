# audio_info.sh — что есть для звука: карты ALSA, устройства, утилиты, модули
echo "== cards"; cat /proc/asound/cards
echo "== pcm"; cat /proc/asound/pcm
echo "== dev/snd"; ls -l /dev/snd
for f in /proc/asound/card*/pcm*p/info; do echo "== $f"; cat $f; done
echo "== tools"; for t in aplay amixer alsamixer tinyplay tinymix tinypcminfo speaker-test bluealsa-aplay; do command -v $t; done
echo "== libs"; ls /usr/lib | grep -i -E "asound|tinyalsa|flac|mad|mpg|ogg|opus|faad|fdk|alac|ffmpeg|avcodec" 
echo "== alsa conf"; ls /usr/share/alsa 2>/dev/null; ls /etc/asound* 2>/dev/null
echo "== sound modules"; lsmod | grep -i -E "sound|codec|es9219|aic|hbc|sa_"
