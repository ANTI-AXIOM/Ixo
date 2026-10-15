// y = a * x + y on the GPU over a million floats, ten times; the sum of y (with cudarc).
use cudarc::driver::{CudaDevice, LaunchAsync, LaunchConfig};
use cudarc::nvrtc::compile_ptx;

const KERNEL: &str = r#"
extern "C" __global__ void saxpy(float a, const float* x, float* y, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) y[i] = a * x[i] + y[i];
}"#;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let n = 1_000_000;
    let dev = CudaDevice::new(0)?;
    dev.load_ptx(compile_ptx(KERNEL)?, "k", &["saxpy"])?;
    let saxpy = dev.get_func("k", "saxpy").unwrap();
    let x = dev.htod_copy((0..n).map(|i| (i % 100) as f32).collect::<Vec<_>>())?;
    let mut y = dev.alloc_zeros::<f32>(n)?;
    for _ in 0..10 {
        unsafe { saxpy.clone().launch(LaunchConfig::for_num_elems(n as u32), (2.0f32, &x, &mut y, n as i32)) }?;
    }
    let sum: f64 = dev.dtoh_sync_copy(&y)?.iter().map(|&v| v as f64).sum();
    println!("{:.1}", sum);
    Ok(())
}
