# img_headers.sh <список> — первые 32 КБ каждой картинки в /tmp/hdr/<n>.bin (для разбора форматов обложек на ПК)
rm -rf /tmp/hdr; mkdir -p /tmp/hdr
n=0
while IFS= read -r f; do
	n=$((n + 1))
	dd if="$f" of=/tmp/hdr/$n.bin bs=32768 count=1 2>/dev/null
	echo "$n	$(stat -c %s "$f")	$f" >> /tmp/hdr/index.txt
done < "$1"
echo "done $n"
