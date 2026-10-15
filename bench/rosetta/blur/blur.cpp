// A 3x3 box blur over a grayscale image, four times; a checksum of the result.
#include <algorithm>
#include <cstdio>
#include <vector>

using Image = std::vector<std::vector<int>>;

Image blur(const Image& img) {
    int h = img.size(), w = img[0].size();
    Image out(h, std::vector<int>(w));
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int total = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) total += img[std::clamp(y + dy, 0, h - 1)][std::clamp(x + dx, 0, w - 1)];
            out[y][x] = total / 9;
        }
    return out;
}

int main() {
    Image img(200, std::vector<int>(200));
    for (int y = 0; y < 200; y++)
        for (int x = 0; x < 200; x++) img[y][x] = (x * 31 + y * 17 + x * y) % 256;
    for (int i = 0; i < 4; i++) img = blur(img);
    long long sum = 0;
    for (auto& row : img)
        for (int v : row) sum += v;
    std::printf("%lld %d\n", sum, img[100][100]);
}
