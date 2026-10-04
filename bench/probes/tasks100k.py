import asyncio
async def sleeper(k):
    await asyncio.sleep(0.05)
    return k
async def main():
    ts = [asyncio.create_task(sleeper(k)) for k in range(100_000)]
    print(sum([await t for t in ts]))
asyncio.run(main())
