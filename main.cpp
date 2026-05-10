#include <fcntl.h>
#include <sys/stat.h>
#include <tiffio.h>
#include <unistd.h>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <tiffio.hxx>
#include <vector>

#include "libraries/cpp-httplib/httplib.h"
#include "libraries/lodepng/lodepng.h"

using namespace std;

namespace fs = std::filesystem;

const double EARTH_PERIMETER = 40075016.68557849;

const uint16 MARGIN_PIXELS = 2;

using color = tuple<uint8, uint8, uint8, uint8>;

bool feq(float a, float b, float epsilon) {
    return fabs(a - b) < epsilon;
}

color render(const vector<vector<float>>& samples) {
    float center = samples[MARGIN_PIXELS][MARGIN_PIXELS];

    if (feq(center, -999.0f, 1.0e-3)) {
        return color(0, 0, 0, 0);
    }

    float totalValue = 0.0f;
    float count = 0;
    for (const auto& row : samples) {
        for (float val : row) {
            if (feq(val, -999.0f, 1.0e-3)) {
                continue;
            }
            totalValue += val;
            ++count;
        }
    }
    float averageValue = totalValue / count;
    float diff = center - averageValue;

    map<float, color> colorTable = {
        {-10.0f, color(64, 64, 255, 255)},   // Hole
        {0.0f, color(0, 0, 0, 255)},         // Neutral
        {12.0f, color(255, 255, 255, 255)},  // Peak
    };

    // Find the two closest keys in the color table
    auto upperIt = colorTable.lower_bound(diff);
    auto lowerIt = upperIt == colorTable.begin() ? upperIt : prev(upperIt);
    if (upperIt == colorTable.end()) {
        upperIt = prev(upperIt);
    }

    // Linear interpolation between the two colors
    float lowerKey = lowerIt->first;
    float upperKey = upperIt->first;
    color lowerColor = lowerIt->second;
    color upperColor = upperIt->second;
    float t = (diff - lowerKey) / (upperKey - lowerKey);

    uint8 r = static_cast<uint8>(get<0>(lowerColor) + t * (get<0>(upperColor) - get<0>(lowerColor)));
    uint8 g = static_cast<uint8>(get<1>(lowerColor) + t * (get<1>(upperColor) - get<1>(lowerColor)));
    uint8 b = static_cast<uint8>(get<2>(lowerColor) + t * (get<2>(upperColor) - get<2>(lowerColor)));
    uint8 a = static_cast<uint8>(get<3>(lowerColor) + t * (get<3>(upperColor) - get<3>(lowerColor)));

    return color(a, b, g, r);
}

struct TileJob {
    mutex m;
    condition_variable cv;
    bool done = false;
};

mutex tileJobsMutex;
unordered_map<string, shared_ptr<TileJob>> tileJobs;

string getTile(uint16 zTMS, uint32 xTMS, uint32 yTMS) {
    const string tilePath = "tiles/" + to_string(zTMS) + "/" + to_string(xTMS) + "/" + to_string(yTMS) + ".png";

    // If file exists, return path.
    if (FILE* file = fopen(tilePath.c_str(), "r")) {
        fclose(file);
        return tilePath;
    }

    shared_ptr<TileJob> job;
    bool iAmGenerator = false;
    {
        lock_guard<mutex> lock(tileJobsMutex);
        auto it = tileJobs.find(tilePath);
        if (it != tileJobs.end()) {
            // There is already a job for this tile, wait for it to finish and return the path.
            job = it->second;
        } else {
            job = make_shared<TileJob>();
            tileJobs[tilePath] = job;
            iAmGenerator = true;
        }
    }
    if (!iAmGenerator) {
        unique_lock<mutex> lock(job->m);

        job->cv.wait(lock, [&] {
            return job->done;
        });

        return tilePath;
    }

    cout << "Generating tile " << tilePath << endl;

    uint32 z = zTMS;
    uint32 x = xTMS;
    uint32 y = (1 << z) - 1 - yTMS;

    double p = pow(2, int32(-z));
    double xMin = EARTH_PERIMETER * (-0.5 + (x - MARGIN_PIXELS / 256.0l) * p);
    double xMax = EARTH_PERIMETER * (-0.5 + (x + 1 + MARGIN_PIXELS / 256.0l) * p);
    double yMin = EARTH_PERIMETER * (0.5 - (y - MARGIN_PIXELS / 256.0l) * p);
    double yMax = EARTH_PERIMETER * (0.5 - (y + 1 + MARGIN_PIXELS / 256.0l) * p);

    // Create named pipe
    stringstream fifoPathStream;
    fifoPathStream << "/tmp/gdal_fifo_" << zTMS << "_" << xTMS << "_" << yTMS << "_" << getpid();
    string fifo_path = fifoPathStream.str();
    if (int ret = mkfifo(fifo_path.c_str(), 0666); ret != 0) {
        throw runtime_error("Failed to create FIFO: " + string(strerror(errno)));
    }

    if (fork() == 0) {
        stringstream ss;
        ss << fixed << setprecision(8);
        ss << xMin << " " << yMin << " " << xMax << " " << yMax;
        string xMinStr, yMinStr, xMaxStr, yMaxStr;
        ss >> xMinStr >> yMinStr >> xMaxStr >> yMaxStr;

        int ret = execlp(
            "gdalwarp",
            "gdalwarp",
            "-te", xMinStr.c_str(), yMinStr.c_str(), xMaxStr.c_str(), yMaxStr.c_str(),
            "-ts", to_string(256 + 2 * MARGIN_PIXELS).c_str(), to_string(256 + 2 * MARGIN_PIXELS).c_str(),
            "vrt/all_3857.vrt",
            fifo_path.c_str(),
            nullptr);
        if (ret == -1) {
            cerr << "Failed to execute gdalwarp: " << strerror(errno) << endl;
            unlink(fifo_path.c_str());
            exit(-1);
        }
    }

    // Read FIFO contents into stringstream
    char buffer[4096];
    stringstream ss;
    int fd = open(fifo_path.c_str(), O_RDONLY);
    if (fd == -1) {
        unlink(fifo_path.c_str());
        throw runtime_error("Failed to open FIFO for reading: " + string(strerror(errno)));
    }
    ssize_t bytesRead;
    while ((bytesRead = read(fd, buffer, sizeof(buffer))) > 0) {
        ss.write(buffer, bytesRead);
    }
    if (bytesRead == -1) {
        close(fd);
        unlink(fifo_path.c_str());
        throw runtime_error("Failed to read from FIFO: " + string(strerror(errno)));
    }
    close(fd);
    istream& is = ss;

    TIFF* tif = TIFFStreamOpen("MemTIFF", &is);
    if (!tif) {
        unlink(fifo_path.c_str());
        throw runtime_error("Failed to open TIFF from FIFO: " + string(strerror(errno)));
    }

    vector<color> image(256 * 256, color(0, 0, 0, 0));

    uint16 bitsPerSample;
    uint16 sampleFormat;
    TIFFGetField(tif, TIFFTAG_BITSPERSAMPLE, &bitsPerSample);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &sampleFormat);

    assert(bitsPerSample == 32);
    assert(sampleFormat == SAMPLEFORMAT_IEEEFP);

    vector<uint8*> buf(256 + 2 * MARGIN_PIXELS, nullptr);
    for (uint32 iTiff = 0; iTiff < 256 + 2 * MARGIN_PIXELS; ++iTiff) {
        buf[iTiff] = (uint8*)_TIFFmalloc(TIFFScanlineSize(tif));
        TIFFReadScanline(tif, buf[iTiff], iTiff);
    }

    for (uint16 i = 0; i < 256; ++i) {
        for (uint16 j = 0; j < 256; ++j) {
            vector<vector<float>> samples(2 * MARGIN_PIXELS + 1, vector<float>(2 * MARGIN_PIXELS + 1));

            for (uint32 iD = 0; iD <= MARGIN_PIXELS * 2; ++iD) {
                for (uint32 jD = 0; jD <= MARGIN_PIXELS * 2; ++jD) {
                    uint32 iTiff = i + iD;
                    uint32 jTiff = j + jD;

                    float elevation = ((float*)buf[jTiff])[iTiff];
                    samples[iD][jD] = elevation;
                }
            }

            color c = render(samples);
            image[(255 - j) * 256 + i] = c;
        }
    }

    fs::create_directories(fs::path(tilePath).parent_path());
    if (uint ret = lodepng::encode(tilePath, (uint8*)image.data(), 256, 256); ret != 0) {
        unlink(fifo_path.c_str());
        throw runtime_error("Failed to encode PNG: " + string(lodepng_error_text(ret)));
    }

    unlink(fifo_path.c_str());

    cout << "Finished generating tile " << tilePath << endl;

    {
        lock_guard<mutex> lock(tileJobsMutex);
        job->done = true;
        job->cv.notify_all();
        tileJobs.erase(tilePath);
    }

    return tilePath;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        cerr << "Usage: " << argv[0] << " <host> <port>" << endl;
        return -1;
    }

    using namespace httplib;

    Server svr;

    svr.Get("/tiles/:z/:x/:y", [&](const Request& req, Response& res) {
        uint16 z = atoi(req.path_params.at("z").c_str());
        uint32 x = atoi(req.path_params.at("x").c_str());
        uint32 y = atoi(req.path_params.at("y").c_str());

        if (!(16 <= z && z <= 19)) {
            res.status = 400;
            res.set_content("Only zoom levels 16 to 19 are supported", "text/plain");
            return;
        }

        const string tilePath = getTile(z, x, y);

        res.set_file_content(tilePath);
    });

    svr.listen(argv[1], atoi(argv[2]));

    return 0;
}
