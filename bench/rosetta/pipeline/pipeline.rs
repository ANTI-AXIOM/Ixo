// A pipeline over channels: a producer, four worker threads, a consumer that sums.
use std::sync::{mpsc, Arc, Mutex};
use std::thread;

fn main() {
    let (job_tx, job_rx) = mpsc::sync_channel::<u64>(256);
    let (res_tx, res_rx) = mpsc::sync_channel::<u64>(256);
    thread::spawn(move || {
        for i in 1..=100_000 {
            job_tx.send(i).unwrap();
        }
    });
    let job_rx = Arc::new(Mutex::new(job_rx));
    for _ in 0..4 {
        let (jobs, results) = (Arc::clone(&job_rx), res_tx.clone());
        thread::spawn(move || loop {
            let n = match jobs.lock().unwrap().recv() {
                Ok(n) => n,
                Err(_) => break,
            };
            results.send(n * n % 1000).unwrap();
        });
    }
    drop(res_tx);
    println!("{}", res_rx.iter().sum::<u64>());
}
