# CPU load for the gfxclk measurements (README "GPU clock"): python3 cpuburn.py <seconds> <processes>, each a tight float loop.
import multiprocessing as mp, time, sys
def burn(t):
    end = time.time() + t
    x = 1.0001
    while time.time() < end:
        for _ in range(100000): x = x * 1.0000001 + 1e-9
if __name__ == '__main__':
    t = float(sys.argv[1]); n = int(sys.argv[2])
    ps = [mp.Process(target=burn, args=(t,)) for _ in range(n)]
    [p.start() for p in ps]; [p.join() for p in ps]
