# Concurrency: run, wait, and use-locks (built-in)

cmm has lightweight threading built into the language with two keywords and a
locking block.

## run / wait

`run <call>` starts the call on its own thread and immediately returns a `Job`
handle. `wait <job>` blocks until that job finishes and yields its result.

```
fn work(n: Int) -> Int {
    r = (n * 2);
    return r;
}

fn main() {
    j1 = run work(10);
    j2 = run work(20);
    a = wait j1;       // 20
    b = wait j2;       // 40
}
```

`run` must be followed by a call expression; `wait` takes a job value.

## use-locks

`use <lock> { ... }` acquires a lock for the duration of the block. Any class
variable can serve as the lock. The lock is released when the block ends —
**including on an early `return` from inside the block**, so locked sections
never leak the lock.

```
class Bank;

@balance: Int;

fn deposit(amount: Int) {
    use @balance {
        @balance = (@balance + amount);
    }   // released here
}

fn main() {
    j1 = run depositMany();
    j2 = run depositMany();
    wait j1;
    wait j2;
    Console.println(@balance);   // deterministic, no lost updates
}
```

Under contention the result is deterministic because mutations to the shared
field happen inside the `use` block. See `examples/Bank.cmm` and
`examples/LockReturn.cmm`.

## Notes

- Threads map to OS threads (pthreads / Win32), abstracted by the runtime.
- Each job's work runs in its own memory arena; the returned value is relocated
  back to the waiter (see the memory model in the [language reference](language.md)).
