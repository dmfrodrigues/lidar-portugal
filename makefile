TIFS := $(wildcard data/MDS-50cm/MDS-50cm-[0-9][0-9][0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9][0-9][0-9].tif)
# TIFS := $(wildcard data/MDS-50cm/MDS-50cm-30[0-9]50[0-9]-[0-9][0-9]-[0-9][0-9][0-9][0-9].tif)

ALL: tile

.SECONDARY:

vrt/all.vrt: $(TIFS)
	gdalbuildvrt $@ $^

vrt/all_3857.vrt: vrt/all.vrt
	gdalwarp -t_srs EPSG:3857 -r cubic $< $@

portion/portion.tiff: vrt/all_3857.vrt
	time gdalwarp -te -760000 5118000 -758662 5119350 $< $@

server: libraries/lodepng/lodepng.cpp main.cpp
	g++ -std=c++17 \
		-o $@ \
		$^ \
		-ltiff -ltiffxx \
		-lpthread

serve: server
	./server 0.0.0.0 8080
