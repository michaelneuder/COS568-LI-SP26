## Task 2 writeup 

Mike Neuder, COS 568 Project, Spring 2026

---

I implemented a simple, two-layered mechanism for this part of the project. I try to take advantage of the fact that LIPP is optimized for lookup and DPGM is optimized for fast insertions. As such, we will maintain a LIPP index to handle the fast path of lookups and a DPGM index to handle efficiently storing newly inserted keys. The mechanism works as follows:
- **Initialize**: The initial data is bulk loaded into the LIPP index, in the same method that is used in the `Build` function of lipp.h.
- **On insert**: Any new keys are inserted into a DPGM index, until the flush threshold (which is configured at compile time as a hyperparameter) is met. 
- **On flush threshold**: When the size of the DPGM index reaches the flush threshold, all of the keys are serially migrated to the LIPP index. 
- **On lookup**: We first try the lookup on the LIPP index with `lipp_.find`. If the key is in the LIPP index, we are in the fast path and can return it immediately. If it is not found in the LIPP index (which means the key was recently inserted), then we search in the DPGM index with `pgm_buffer_.find` and return it.

The goal is that most of the lookups will be on the fast path of the LIPP index and the flushing mechanism prevents the DPGM index from getting too big. The obvious downside of this approach is that the flushing mechanism is sequential and blocking. That is, the insert that triggers the flush is fully blocked on the bulk insertion of all the values that were in the DPGM index into the LIPP index. This is further exacerbated by the fact that the LIPP insertions are slow (remember it is optimized for lookups), so doing this process sequentially is pretty costly. Still, this approach helped me get a feel for how to work with the two-index approach and is a good first pass. 

We also have to test different hyperparameter configurations in the benchmarking. For this task, just to get started, I tried three different flush sizes 1000, 10000, 100000 and for the DPGM index used both `BranchingBinarySearch` and `LinearSearch` both with error size 128. For part 3, when I am explicitly trying to beat the LIPP benchmark, I may try different configurations that are more accurately tuned for the different workload types. For the DPGM index, I try all the different hyperparameter configurations that were provided for the task 1 benchmarking.

The figures below show the resulting data for just the facebook dataset for DPGM, LIPP, and HYBRID. 

![](./task2-10-tput.png)

This figure shows that in the 10% insert regime (meaning a majority of the workload is lookups, which favors the LIPP index), the hybrid approach is right in the middle of the other two approaches in performance. This makes sense as some of the lookups are going to go through the DPGM buffer if they were recently inserted, but at least a good chunk of the lookups will be on the LIPP fast path. 

![](./task2-10-size.png)

This figure shows the index size for each of the methods on this workload. We see that the hybrid approach is equally large as LIPP because we are still using LIPP for a lot of the keys that we bulk loaded at the beginning and LIPP also has to handle any flushed keys from the DPGM index. DPGM is a much smaller index generally. 

![](./task2-90-tput.png)

This figure shows that in the 90% insert regime (meaning a majority of the workload is is inserts, which favors the DPGM index), the hybrid approach is worse than both the DPGM and LIPP indices. This makes sense because our naïve flushing strategy is kind of giving us the worst of both worlds in that we repeatedly are inserting first into the DPGM index, but then continually flushing into the LIPP index after. Further, since most of the the lookups will be on recently inserted keys, we often times will have to fallback to the slow path of reading from both LIPP and then DPGM. As such, many of both the lookups and inserts will be done twice under this workload. When we start tuning for part 3, we will need to address this issue. 

![](./task2-90-size.png)

Again, we see the sizes of the indices are similar to the 10% insert workload. The rationale here is the same. The hybrid index ends up inserting most of the keys into the LIPP index anyway, so we don't benefit from the small index size of DPGM too much due to all the flushing. 