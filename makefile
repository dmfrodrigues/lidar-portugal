TIFS := $(wildcard data/MDS-50cm/MDS-50cm-[0-9][0-9][0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9][0-9][0-9].tif)

ALL: all_3857.vrt

.SECONDARY:

vrt/all.vrt: $(TIFS)
	gdalbuildvrt $@ $^

vrt/all_3857.vrt: vrt/all.vrt
	gdalwarp -t_srs EPSG:3857 -r cubic $< $@

server: libraries/lodepng/lodepng.cpp main.cpp
	g++ -std=c++17 \
		-o $@ \
		$^ \
		-ltiff -ltiffxx \
		-lpthread

serve: server
	./server 0.0.0.0 8080
