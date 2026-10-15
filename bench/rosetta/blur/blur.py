# A 3x3 box blur over a grayscale image, four times; a checksum of the result.
def blur(img):
    h, w = len(img), len(img[0])
    return [
        [
            sum(img[min(max(y + dy, 0), h - 1)][min(max(x + dx, 0), w - 1)] for dy in (-1, 0, 1) for dx in (-1, 0, 1)) // 9
            for x in range(w)
        ]
        for y in range(h)
    ]


img = [[(x * 31 + y * 17 + x * y) % 256 for x in range(200)] for y in range(200)]
for _ in range(4):
    img = blur(img)
print(sum(map(sum, img)), img[100][100])
