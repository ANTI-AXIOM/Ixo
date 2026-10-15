// A 3x3 box blur over a grayscale image, four times; a checksum of the result.
fn blur(img: &[Vec<i64>]) -> Vec<Vec<i64>> {
    let (h, w) = (img.len() as i64, img[0].len() as i64);
    (0..h)
        .map(|y| {
            (0..w)
                .map(|x| {
                    let mut total = 0;
                    for dy in -1..=1 {
                        for dx in -1..=1 {
                            total += img[(y + dy).clamp(0, h - 1) as usize][(x + dx).clamp(0, w - 1) as usize];
                        }
                    }
                    total / 9
                })
                .collect()
        })
        .collect()
}

fn main() {
    let mut img: Vec<Vec<i64>> = (0..200).map(|y| (0..200).map(|x| (x * 31 + y * 17 + x * y) % 256).collect()).collect();
    for _ in 0..4 {
        img = blur(&img);
    }
    println!("{} {}", img.iter().flatten().sum::<i64>(), img[100][100]);
}
