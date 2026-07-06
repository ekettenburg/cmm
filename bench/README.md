# JSON benchmark

Measures the native JSON parser/serializer throughput on a ~16.7 MB document
(200k objects). Build against the runtime and run:

```
gcc -O2 -std=c99 -I ../runtime json_bench.c ../runtime/cmm_runtime.c -o jbench -lm -lpthread
./jbench
```

Representative result (this build): decode ~90 MB/s, encode ~120 MB/s.
