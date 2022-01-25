## ld64 optimize design
### ld64简介
ld64整体执行流程
- Command line processing
- Parsing input files
- Resolving
- Passes/Optimizations
- Generate output file

### 现有可优化点
只在**Parsing input files**用到多线程处理，后续过程是单任务处理，目前的开发/构建机多核机器，适合做并行化优化
- Xcode Target粒度**Input files**和**Build Setting**均未变化，可以不执行Link；**需要确认执行条件；**
- Resolving阶段**Symbol resolution**并行化，符号hash函数，多核机器可以尽可能的并行执行；
- Resolving阶段是**CPU-bounded**任务，并行执行**I/O-bounded**任务（如Header文件，资源的拷贝）；**需要确认Xcode builtin-copy是否可以忽略相同文件的拷贝**；
- **UUID**的生成并行计算归并产生结果；
- **Pass**阶段整体流程为串行流水任务，但是Pass内部Atom处理为独立子任务，可以并行化优化。

### ld64 incremental link

Mach-O section预留Padding区域，下一次link过程插入新修改的Atom，复用已存在的Mach-O，对新增Atom进行Fixups。

步骤：
- 判断Input files变化和不变，区分处理；如果满足Incremental link，读取已存在的Mach-O；如果不满足按正常的Link执行过程；
- 读取已有的Mach-O文件，找到修改文件(.o)的变化的**Atom**在Mach-O的section位置；
- 在对应Mach-O section的尾部padding，增加变化的**Atom**；
- **Perform Fixups**将修改前的旧**Atom**的relocations全部指向新增**Atom**的位置

#### Add atom

#### Delete atom

#### Update atom




