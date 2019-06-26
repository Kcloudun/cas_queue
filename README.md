# cas_queue

// hahaha

concurrence lock-free block and non-blocking queues

并发无锁阻塞和非阻塞队列

队列基于Linux下gcc原子API实现多线程间无锁生产/消费数据

使用者include “cas_queue.hxx”即可

cas_queue.hxx中包含八个队列类，分为阻塞和非阻塞


阻塞队列：

	1）CasQueueMPMC：多生产多消费场景使用
	
	2）CasQueueMPOC：多生产单消费场景使用
	
	3）CasQueueOPMC：单生产多消费场景使用
	
	4）CasQueueOPOC：单生产单消费场景使用
	
	
非阻塞队列：

	1）CasQueueNoBlockMPMC：多生产多消费场景使用
	
	2）CasQueueNoBlockMPOC：多生产单消费场景使用
	
	3）CasQueueNoBlockOPMC：单生产多消费场景使用
	
	4）CasQueueNoBlockOPOC：单生产单消费场景使用
	
  
如使用者要传输复杂的struct或class数据类型需要重载等于号操作符。

每个类中都有Product和Consume方法，针对不同的场景使用不同的类，不要用错哦。

每个类的测试例子在example。
