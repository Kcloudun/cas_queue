#ifndef __CAS_QUEUE__
#define __CAS_QUEUE__

#include <iostream>
#include <string>
#include <cmath>
#include <pthread.h>
#include <stdlib.h>

using namespace std;

// 多生产者多消费者阻塞队列
template <class T>
class CasQueueMPMC
{
	public:
		CasQueueMPMC()
		{
			size = 16384;
			product_index = consume_index = 0;

			p_queue = new ENTRY [size];

			// 初始化队列
			for (int ii = 0; ii < size; ++ii)
			{
				p_queue[ii].e_state = EMPTY;

				p_queue[ii].f_door = FRONT_DOOR_OPEN;
				p_queue[ii].b_door = BACK_DOOR_OPEN;

				pthread_mutex_init(&p_queue[ii].product_mutex, NULL);
				pthread_cond_init(&p_queue[ii].product_cond, NULL);
				p_queue[ii].product_awake_flag = false;
				p_queue[ii].p_wait = P_INIT;

				pthread_mutex_init(&p_queue[ii].consume_mutex, NULL);
				pthread_cond_init(&p_queue[ii].consume_cond, NULL);
				p_queue[ii].consume_awake_flag = false;
				p_queue[ii].c_wait = C_INIT;
			}
		}

		CasQueueMPMC(int queue_size)
		{
			product_index = consume_index = 0;

			size = pow(2, (ceil(log2(queue_size))));
			p_queue = new ENTRY [size];

			// 初始化队列
			for (int ii = 0; ii < size; ++ii)
			{
				p_queue[ii].e_state = EMPTY;

				p_queue[ii].f_door = FRONT_DOOR_OPEN;
				p_queue[ii].b_door = BACK_DOOR_OPEN;

				pthread_mutex_init(&p_queue[ii].product_mutex, NULL);
				pthread_cond_init(&p_queue[ii].product_cond, NULL);
				p_queue[ii].product_awake_flag = false;
				p_queue[ii].p_wait = P_INIT;

				pthread_mutex_init(&p_queue[ii].consume_mutex, NULL);
				pthread_cond_init(&p_queue[ii].consume_cond, NULL);
				p_queue[ii].consume_awake_flag = false;
				p_queue[ii].c_wait = C_INIT;
			}
		}

		virtual ~CasQueueMPMC()
		{
			delete [] p_queue;
		}

		void Product(T &t_product)
		{
			// product_index为类成员变量，表示生产索引，利用unsigned long类型达到最大值后循环归零的特性递增生产索引, 为多个生产者原子性分配生产entry位置
			unsigned long current_product_index = __sync_fetch_and_add(&product_index, 1);
			current_product_index &= (size - 1);  // 代替取模操作提升性能，size为队列初始化entry个数，必须为2的N次幂

			// loop_entry_product 为防止多个生产者时，有些生产者速度快甩其它生产者一圈后与速度慢的生产者进入了同一个entry，此时快的生产者必须轮询等待慢的生产者生产完毕，而不能越过该entry，如果越过该entry可能导致在该entry的消费者永远阻塞
loop_entry_product:
			// 生产者进前门，使用cas的原因为有可能多个生产者的情况下，防止有速度快的生产者套圈后又回到了这个位置的entry后导致有多个生产者同时进入一个entry
			bool is_enter = __sync_bool_compare_and_swap(&p_queue[current_product_index].f_door, FRONT_DOOR_OPEN, FRONT_DOOR_CLOSE);
			if (true == is_enter)
			{
				// 判断entry状态，如果为空则置为生产状态
				bool is_product = __sync_bool_compare_and_swap(&p_queue[current_product_index].e_state, EMPTY, PRODUCT);
				if (true == is_product)
				{
					// 生产数据
					p_queue[current_product_index].data = t_product;
					p_queue[current_product_index].p_wait = P_INIT;  // 每次生产完数据需要将p_wait初始化

					__AwakeConsume(current_product_index);  // 判断是否唤醒消费者

					// 打开前门
					__sync_lock_test_and_set(&p_queue[current_product_index].f_door, FRONT_DOOR_OPEN);

					return;
				}
				else  // 该entry已经有数据 或 有消费者正在消费数据
				{
					bool is_wait = __sync_bool_compare_and_swap(&p_queue[current_product_index].p_wait, P_INIT, P_WAIT);
					if (true == is_wait)  // 等待消费者唤醒
					{
						pthread_mutex_lock(&p_queue[current_product_index].product_mutex);
						while (!p_queue[current_product_index].product_awake_flag)
							pthread_cond_wait(&p_queue[current_product_index].product_cond, &p_queue[current_product_index].product_mutex);
						pthread_mutex_unlock(&p_queue[current_product_index].product_mutex);

						// 生产数据
						p_queue[current_product_index].data = t_product;
						p_queue[current_product_index].p_wait = P_INIT;
						p_queue[current_product_index].product_awake_flag = false;

						__AwakeConsume(current_product_index);

						// 打开前门
						__sync_lock_test_and_set(&p_queue[current_product_index].f_door, FRONT_DOOR_OPEN);

						return;
					}
					else  // p_wait已经被消费者置为2（忽略），说明消费者已经消费完毕，但e_state不一定被及时置为0（空），需要进行轮询式判断
					{
loop_product:
						bool is_product = __sync_bool_compare_and_swap(&p_queue[current_product_index].e_state, EMPTY, PRODUCT);
						if (true == is_product)
						{
							// 生产数据
							p_queue[current_product_index].data = t_product;
							p_queue[current_product_index].p_wait = P_INIT;

							__AwakeConsume(current_product_index);

							// 打开前门
							__sync_lock_test_and_set(&p_queue[current_product_index].f_door, FRONT_DOOR_OPEN);

							return;	
						}
						else  // 此种情况极少发生，一旦发生循环判断
						{
							goto loop_product;
						}
					}
				}
			}
			else  // 已经有生产者进入，继续获取entry位置
			{
				goto loop_entry_product;
			}
		}

		void Consume(T &t_consume)
		{
			unsigned long current_consume_index = __sync_fetch_and_add(&consume_index, 1);
			current_consume_index &= (size - 1);

loop_entry_consume:  // 防止多个消费者时，有些消费者速度快甩其它消费者一圈后与速度慢的消费者进入了同一个entry，此时快的消费者必须轮询等待慢的消费者消费完毕，而不能越过该entry
			// 进后门
			bool is_enter = __sync_bool_compare_and_swap(&p_queue[current_consume_index].b_door, BACK_DOOR_OPEN, BACK_DOOR_CLOSE);
			if (true == is_enter)
			{
				// 判断entry状态
				bool is_consume = __sync_bool_compare_and_swap(&p_queue[current_consume_index].e_state, FULL, CONSUME);
				if (true == is_consume)
				{
					// 消费数据
					t_consume = p_queue[current_consume_index].data;
					p_queue[current_consume_index].c_wait = C_INIT;

					__AwakeProduct(current_consume_index);

					// 打开后门
					__sync_lock_test_and_set(&p_queue[current_consume_index].b_door, BACK_DOOR_OPEN);

					return;
				}
				else  // 该entry已经没有数据 或 有生产者正在生产数据
				{
					bool is_wait = __sync_bool_compare_and_swap(&p_queue[current_consume_index].c_wait, C_INIT, C_WAIT);
					if (true == is_wait)  // 等待生产者唤醒
					{
						pthread_mutex_lock(&p_queue[current_consume_index].consume_mutex);
						while (!p_queue[current_consume_index].consume_awake_flag)
							pthread_cond_wait(&p_queue[current_consume_index].consume_cond, &p_queue[current_consume_index].consume_mutex);
						pthread_mutex_unlock(&p_queue[current_consume_index].consume_mutex);

						// 消费数据
						t_consume = p_queue[current_consume_index].data;
						p_queue[current_consume_index].c_wait = C_INIT;
						p_queue[current_consume_index].consume_awake_flag = false;

						__AwakeProduct(current_consume_index);

						// 打开后门
						__sync_lock_test_and_set(&p_queue[current_consume_index].b_door, BACK_DOOR_OPEN);

						return;
					}
					else  // c_wait已经被生产者置为2（忽略），说明生产者已经生产完毕，但e_state不一定被及时置为2（满），需要进行轮询式判断
					{
loop_consume:
						bool is_consume = __sync_bool_compare_and_swap(&p_queue[current_consume_index].e_state, FULL, CONSUME);
						if (true == is_consume)
						{
							// 消费数据
							t_consume = p_queue[current_consume_index].data;
							p_queue[current_consume_index].c_wait = C_INIT;

							__AwakeProduct(current_consume_index);

							// 打开后门
							__sync_lock_test_and_set(&p_queue[current_consume_index].b_door, BACK_DOOR_OPEN);

							return;	
						}
						else  // 此种情况极少发生，一旦发生循环判断
						{
							goto loop_consume;
						}
					}
				}
			}
			else  // 已经有消费者进入，继续获取entry位置
			{
				goto loop_entry_consume;
			}
		}

	private:
		enum entry_state {EMPTY = 0, PRODUCT, FULL, CONSUME};
		enum front_door {FRONT_DOOR_OPEN = 0, FRONT_DOOR_CLOSE};
		enum back_door {BACK_DOOR_OPEN = 0, BACK_DOOR_CLOSE};
		enum product_wait {P_INIT = 0, P_WAIT, P_IGNORE};
		enum consume_wait {C_INIT = 0, C_WAIT, C_IGNORE};

		/*每个队列由N个entry组成，每个entry的数据结构如下*/
		typedef struct 
		{
			T data;  // 实际生产、消费的数据，可以为任意类型，对于复杂类型必须自己重置等于号操作符

			/* 
			 * 	【entry的四种状态】
			 *
			 * EMPTY	0: 表示entry为空可以生产
			 * PRODUCT	1: 表示entry有生产者正在生产
			 * FULL		2: 表示entry有数据可以消费
			 * CONSUME	3: 表示有消费者正在消费
			 *
			 */
			entry_state e_state;

			front_door f_door;	// 前门，防止多个生产者同时进入同一个entry，只有在多个生产者模式下此标识才有作用，因为有的生产者领先其它生产者套圈的情况出现
			back_door b_door;	// 后门，防止多个消费者同时进入同一个entry，只有在多个消费者模式下此标识才有作用，因为有的消费者领先其它消费者套圈的情况出现

			pthread_mutex_t product_mutex;
			pthread_cond_t product_cond;	// 唤醒生产者条件变量
			bool product_awake_flag;	// true：表示唤醒消费者
			/*
			 * 	【p_wait 表示生产者是否等待消费者唤醒，CAS阻塞队列的精髓】
			 *
			 * P_INIT	0: 初始值也即抢占值
			 * P_WAIT	1: 生产者抢占成功后置，表示生产者需>要阻塞在该entry上等待消费者唤醒
			 * P_IGNORE	2: 消费者抢占成功后置，表示忽略本次唤醒生产者
			 *
			 */
			product_wait p_wait;

			pthread_mutex_t consume_mutex;
			pthread_cond_t consume_cond;	// 唤醒消费者条件变量
			bool consume_awake_flag;	// true：表示唤醒生产者
			/*
			 *	 【c_wait 表示消费者是否等待生产者唤醒，CAS阻塞队列的精髓】
			 *
			 * C_INIT	0: 初始值也即抢占值
			 * C_WAIT	1: 消费者抢占成功后置，表示消费者需>要阻塞在该entry上等待生产者唤醒
			 * C_IGNORE	2: 生产者抢占成功后置，表示忽略本次唤醒消费者
			 */
			consume_wait c_wait;
		} ENTRY;

		ENTRY *p_queue __attribute__((aligned(64)));
		unsigned int size __attribute__((aligned(64)));
		unsigned long product_index __attribute__((aligned(64)));
		unsigned long consume_index __attribute__((aligned(64)));

		inline void __AwakeConsume(unsigned long __current_product_index)
		{
			// 判断是忽略消费者还是唤醒消费者
			bool is_ignore = __sync_bool_compare_and_swap(&p_queue[__current_product_index].c_wait, C_INIT, C_IGNORE);
			if (true == is_ignore)  // 忽略消费者
			{
				__sync_lock_test_and_set(&p_queue[__current_product_index].e_state, FULL);
			}
			else  // 唤醒消费者
			{			
				p_queue[__current_product_index].e_state = FULL;

				pthread_mutex_lock(&p_queue[__current_product_index].consume_mutex);
				p_queue[__current_product_index].consume_awake_flag = true;
				pthread_cond_signal(&p_queue[__current_product_index].consume_cond);
				pthread_mutex_unlock(&p_queue[__current_product_index].consume_mutex);
			}

			return;
		}

		inline void __AwakeProduct(unsigned long __current_consume_index)
		{
			// 判断是忽略生产者还是唤醒生产者
			bool is_ignore = __sync_bool_compare_and_swap(&p_queue[__current_consume_index].p_wait, P_INIT, P_IGNORE);
			if (true == is_ignore)  // 忽略生产者
			{
				__sync_lock_test_and_set(&p_queue[__current_consume_index].e_state, EMPTY);
			}
			else  // 唤醒生产者
			{			
				p_queue[__current_consume_index].e_state = EMPTY;

				pthread_mutex_lock(&p_queue[__current_consume_index].product_mutex);
				p_queue[__current_consume_index].product_awake_flag = true;
				pthread_cond_signal(&p_queue[__current_consume_index].product_cond);
				pthread_mutex_unlock(&p_queue[__current_consume_index].product_mutex);
			}

			return;
		}
};

// 多生产者单消费者阻塞队列
template <class T>
class CasQueueMPOC
{
	public:
		CasQueueMPOC()
		{
			size = 16384;
			product_index = consume_index = 0;

			p_queue = new ENTRY [size];

			// 初始化队列
			for (int ii = 0; ii < size; ++ii)
			{
				p_queue[ii].e_state = EMPTY;

				p_queue[ii].f_door = FRONT_DOOR_OPEN;

				pthread_mutex_init(&p_queue[ii].product_mutex, NULL);
				pthread_cond_init(&p_queue[ii].product_cond, NULL);
				p_queue[ii].product_awake_flag = false;
				p_queue[ii].p_wait = P_INIT;

				pthread_mutex_init(&p_queue[ii].consume_mutex, NULL);
				pthread_cond_init(&p_queue[ii].consume_cond, NULL);
				p_queue[ii].consume_awake_flag = false;
				p_queue[ii].c_wait = C_INIT;
			}
		}

		CasQueueMPOC(int queue_size)
		{
			product_index = consume_index = 0;

			size = pow(2, (ceil(log2(queue_size))));
			p_queue = new ENTRY [size];

			// 初始化队列
			for (int ii = 0; ii < size; ++ii)
			{
				p_queue[ii].e_state = EMPTY;

				p_queue[ii].f_door = FRONT_DOOR_OPEN;

				pthread_mutex_init(&p_queue[ii].product_mutex, NULL);
				pthread_cond_init(&p_queue[ii].product_cond, NULL);
				p_queue[ii].product_awake_flag = false;
				p_queue[ii].p_wait = P_INIT;

				pthread_mutex_init(&p_queue[ii].consume_mutex, NULL);
				pthread_cond_init(&p_queue[ii].consume_cond, NULL);
				p_queue[ii].consume_awake_flag = false;
				p_queue[ii].c_wait = C_INIT;
			}
		}

		virtual ~CasQueueMPOC()
		{
			delete [] p_queue;
		}

		void Product(T &t_product)
		{
			unsigned long current_product_index = __sync_fetch_and_add(&product_index, 1);
			current_product_index &= (size - 1);

loop_entry_product:
			// 进前门
			bool is_enter = __sync_bool_compare_and_swap(&p_queue[current_product_index].f_door, FRONT_DOOR_OPEN, FRONT_DOOR_CLOSE);
			if (true == is_enter)
			{
				// 判断entry状态
				bool is_product = __sync_bool_compare_and_swap(&p_queue[current_product_index].e_state, EMPTY, PRODUCT);
				if (true == is_product)
				{
					// 生产数据
					p_queue[current_product_index].data = t_product;
					p_queue[current_product_index].p_wait = P_INIT;

					__AwakeConsume(current_product_index);

					// 打开前门
					__sync_lock_test_and_set(&p_queue[current_product_index].f_door, FRONT_DOOR_OPEN);

					return;
				}
				else  // 该entry已经有数据 或 有消费者正在消费数据
				{
					bool is_wait = __sync_bool_compare_and_swap(&p_queue[current_product_index].p_wait, P_INIT, P_WAIT);
					if (true == is_wait)  // 等待消费者唤醒
					{
						pthread_mutex_lock(&p_queue[current_product_index].product_mutex);
						while (!p_queue[current_product_index].product_awake_flag)
							pthread_cond_wait(&p_queue[current_product_index].product_cond, &p_queue[current_product_index].product_mutex);
						pthread_mutex_unlock(&p_queue[current_product_index].product_mutex);

						// 生产数据
						p_queue[current_product_index].data = t_product;
						p_queue[current_product_index].p_wait = P_INIT;
						p_queue[current_product_index].product_awake_flag = false;

						__AwakeConsume(current_product_index);

						// 打开前门
						__sync_lock_test_and_set(&p_queue[current_product_index].f_door, FRONT_DOOR_OPEN);

						return;
					}
					else  // p_wait已经被消费者置为2（忽略），说明消费者已经消费完毕，但e_state不一定被及时置为0（空），需要进行轮询式判断
					{
loop_product:
						bool is_product = __sync_bool_compare_and_swap(&p_queue[current_product_index].e_state, EMPTY, PRODUCT);
						if (true == is_product)
						{
							// 生产数据
							p_queue[current_product_index].data = t_product;
							p_queue[current_product_index].p_wait = P_INIT;

							__AwakeConsume(current_product_index);

							// 打开前门
							__sync_lock_test_and_set(&p_queue[current_product_index].f_door, FRONT_DOOR_OPEN);

							return;	
						}
						else  // 此种情况极少发生，一旦发生循环判断
						{
							goto loop_product;
						}
					}
				}
			}
			else  // 已经有生产者进入，继续获取entry位置
			{
				goto loop_entry_product;
			}
		}

		void Consume(T &t_consume)
		{
			unsigned long current_consume_index = consume_index++;
			current_consume_index &= (size - 1);

			// 判断entry状态
			if (true == __sync_bool_compare_and_swap(&p_queue[current_consume_index].e_state, FULL, CONSUME))
			{
				// 消费数据
				t_consume = p_queue[current_consume_index].data;
				p_queue[current_consume_index].c_wait = C_INIT;

				__AwakeProduct(current_consume_index);

				return;
			}
			else  // 该entry已经没有数据 或 有生产者正在生产数据
			{
				bool is_wait = __sync_bool_compare_and_swap(&p_queue[current_consume_index].c_wait, C_INIT, C_WAIT);
				if (true == is_wait)  // 等待生产者唤醒
				{
					pthread_mutex_lock(&p_queue[current_consume_index].consume_mutex);
					while (!p_queue[current_consume_index].consume_awake_flag)
						pthread_cond_wait(&p_queue[current_consume_index].consume_cond, &p_queue[current_consume_index].consume_mutex);
					pthread_mutex_unlock(&p_queue[current_consume_index].consume_mutex);

					// 消费数据
					t_consume = p_queue[current_consume_index].data;
					p_queue[current_consume_index].c_wait = C_INIT;
					p_queue[current_consume_index].consume_awake_flag = false;

					__AwakeProduct(current_consume_index);

					return;
				}
				else  // c_wait已经被生产者置为2（忽略），说明生产者已经生产完毕，但e_state不一定被及时置为2（满），需要进行轮询式判断
				{
loop_consume:
					if (true == __sync_bool_compare_and_swap(&p_queue[current_consume_index].e_state, FULL, CONSUME))
					{
						// 消费数据
						t_consume = p_queue[current_consume_index].data;
						p_queue[current_consume_index].c_wait = C_INIT;

						__AwakeProduct(current_consume_index);

						return;
					}
					else  // 此种情况极少发生，一旦发生循环判断
					{
						goto loop_consume;
					}
				}
			}
		}

	private:
		enum entry_state {EMPTY = 0, PRODUCT, FULL, CONSUME};
		enum front_door {FRONT_DOOR_OPEN = 0, FRONT_DOOR_CLOSE};
		enum product_wait {P_INIT = 0, P_WAIT, P_IGNORE};
		enum consume_wait {C_INIT = 0, C_WAIT, C_IGNORE};

		typedef struct 
		{
			T data;

			entry_state e_state;

			front_door f_door;

			pthread_mutex_t product_mutex;
			pthread_cond_t product_cond;
			bool product_awake_flag;  // true：表示生产者等待消费者唤醒，false：表示生产者已被消费者唤醒
			product_wait p_wait;

			pthread_mutex_t consume_mutex;
			pthread_cond_t consume_cond;
			bool consume_awake_flag;  // true：表示消费者等待生产者唤醒，false：表示消费者已被生产者唤醒
			consume_wait c_wait;
		} ENTRY;

		ENTRY *p_queue __attribute__((aligned(64)));
		unsigned int size __attribute__((aligned(64)));
		unsigned long product_index __attribute__((aligned(64)));
		unsigned long consume_index __attribute__((aligned(64)));

		inline void __AwakeConsume(unsigned long __current_product_index)
		{
			// 判断是忽略消费者还是唤醒消费者
			bool is_ignore = __sync_bool_compare_and_swap(&p_queue[__current_product_index].c_wait, C_INIT, C_IGNORE);
			if (true == is_ignore)  // 忽略消费者
			{
				__sync_lock_test_and_set(&p_queue[__current_product_index].e_state, FULL);
			}
			else  // 唤醒消费者
			{			
				p_queue[__current_product_index].e_state = FULL;

				pthread_mutex_lock(&p_queue[__current_product_index].consume_mutex);
				p_queue[__current_product_index].consume_awake_flag = true;
				pthread_cond_signal(&p_queue[__current_product_index].consume_cond);
				pthread_mutex_unlock(&p_queue[__current_product_index].consume_mutex);
			}

			return;
		}

		inline void __AwakeProduct(unsigned long __current_consume_index)
		{
			// 判断是忽略生产者还是唤醒生产者
			bool is_ignore = __sync_bool_compare_and_swap(&p_queue[__current_consume_index].p_wait, P_INIT, P_IGNORE);
			if (true == is_ignore)  // 忽略生产者
			{
				__sync_lock_test_and_set(&p_queue[__current_consume_index].e_state, EMPTY);
			}
			else  // 唤醒生产者
			{			
				p_queue[__current_consume_index].e_state = EMPTY;

				pthread_mutex_lock(&p_queue[__current_consume_index].product_mutex);
				p_queue[__current_consume_index].product_awake_flag = true;
				pthread_cond_signal(&p_queue[__current_consume_index].product_cond);
				pthread_mutex_unlock(&p_queue[__current_consume_index].product_mutex);
			}

			return;
		}
};

// 单生产者多消费者阻塞队列
template <class T>
class CasQueueOPMC
{
	public:
		CasQueueOPMC()
		{
			size = 16384;
			product_index = consume_index = 0;

			p_queue = new ENTRY [size];

			// 初始化队列
			for (int ii = 0; ii < size; ++ii)
			{
				p_queue[ii].e_state = EMPTY;

				p_queue[ii].b_door = BACK_DOOR_OPEN;

				pthread_mutex_init(&p_queue[ii].product_mutex, NULL);
				pthread_cond_init(&p_queue[ii].product_cond, NULL);
				p_queue[ii].product_awake_flag = false;
				p_queue[ii].p_wait = P_INIT;

				pthread_mutex_init(&p_queue[ii].consume_mutex, NULL);
				pthread_cond_init(&p_queue[ii].consume_cond, NULL);
				p_queue[ii].consume_awake_flag = false;
				p_queue[ii].c_wait = C_INIT;
			}
		}

		CasQueueOPMC(int queue_size)
		{
			product_index = consume_index = 0;

			size = pow(2, (ceil(log2(queue_size))));
			p_queue = new ENTRY [size];

			// 初始化队列
			for (int ii = 0; ii < size; ++ii)
			{
				p_queue[ii].e_state = EMPTY;

				p_queue[ii].b_door = BACK_DOOR_OPEN;

				pthread_mutex_init(&p_queue[ii].product_mutex, NULL);
				pthread_cond_init(&p_queue[ii].product_cond, NULL);
				p_queue[ii].product_awake_flag = false;
				p_queue[ii].p_wait = P_INIT;

				pthread_mutex_init(&p_queue[ii].consume_mutex, NULL);
				pthread_cond_init(&p_queue[ii].consume_cond, NULL);
				p_queue[ii].consume_awake_flag = false;
				p_queue[ii].c_wait = C_INIT;
			}
		}

		virtual ~CasQueueOPMC()
		{
			delete [] p_queue;
		}

		void Product(T &t_product)
		{
			unsigned long current_product_index = product_index++;
			current_product_index &= (size - 1);

			// 判断entry状态
			if (true == __sync_bool_compare_and_swap(&p_queue[current_product_index].e_state, EMPTY, PRODUCT))
			{
				// 生产数据
				p_queue[current_product_index].data = t_product;
				p_queue[current_product_index].p_wait = P_INIT;

				__AwakeConsume(current_product_index);

				return;
			}
			else  // 该entry已经有数据 或 有消费者正在消费数据
			{
				if (true == __sync_bool_compare_and_swap(&p_queue[current_product_index].p_wait, P_INIT, P_WAIT))
				{
					pthread_mutex_lock(&p_queue[current_product_index].product_mutex);
					while (!p_queue[current_product_index].product_awake_flag)
						pthread_cond_wait(&p_queue[current_product_index].product_cond, &p_queue[current_product_index].product_mutex);
					pthread_mutex_unlock(&p_queue[current_product_index].product_mutex);

					// 生产数据
					p_queue[current_product_index].data = t_product;
					p_queue[current_product_index].p_wait = P_INIT;
					p_queue[current_product_index].product_awake_flag = false;

					__AwakeConsume(current_product_index);

					return;
				}
				else  // p_wait已经被消费者置为2（忽略），说明消费者已经消费完毕，但e_state不一定被及时置为0（空），需要进行轮询式判断
				{
loop_product:
					bool is_product = __sync_bool_compare_and_swap(&p_queue[current_product_index].e_state, EMPTY, PRODUCT);
					if (true == is_product)
					{
						// 生产数据
						p_queue[current_product_index].data = t_product;
						p_queue[current_product_index].p_wait = P_INIT;

						__AwakeConsume(current_product_index);

						return;
					}
					else  // 此种情况极少发生，一旦发生循环判断
					{
						goto loop_product;
					}
				}
			}
		}

		void Consume(T &t_consume)
		{
			unsigned long current_consume_index = __sync_fetch_and_add(&consume_index, 1);
			current_consume_index &= (size - 1);

loop_entry_consume:  // 防止多个消费者时，有些消费者速度快甩其它消费者一圈后与速度慢的消费者进入了同一个entry，此时快的消费者必须轮询等待慢的消费者消费完毕，而不能越过该entry
			// 进后门
			bool is_enter = __sync_bool_compare_and_swap(&p_queue[current_consume_index].b_door, BACK_DOOR_OPEN, BACK_DOOR_CLOSE);
			if (true == is_enter)
			{
				// 判断entry状态
				bool is_consume = __sync_bool_compare_and_swap(&p_queue[current_consume_index].e_state, FULL, CONSUME);
				if (true == is_consume)
				{
					// 消费数据
					t_consume = p_queue[current_consume_index].data;
					p_queue[current_consume_index].c_wait = C_INIT;

					__AwakeProduct(current_consume_index);

					// 打开后门
					__sync_lock_test_and_set(&p_queue[current_consume_index].b_door, BACK_DOOR_OPEN);

					return;
				}
				else  // 该entry已经没有数据 或 有生产者正在生产数据
				{
					bool is_wait = __sync_bool_compare_and_swap(&p_queue[current_consume_index].c_wait, C_INIT, C_WAIT);
					if (true == is_wait)  // 等待生产者唤醒
					{
						pthread_mutex_lock(&p_queue[current_consume_index].consume_mutex);
						while (!p_queue[current_consume_index].consume_awake_flag)
							pthread_cond_wait(&p_queue[current_consume_index].consume_cond, &p_queue[current_consume_index].consume_mutex);
						pthread_mutex_unlock(&p_queue[current_consume_index].consume_mutex);

						// 消费数据
						t_consume = p_queue[current_consume_index].data;
						p_queue[current_consume_index].c_wait = C_INIT;
						p_queue[current_consume_index].consume_awake_flag = false;

						__AwakeProduct(current_consume_index);

						// 打开后门
						__sync_lock_test_and_set(&p_queue[current_consume_index].b_door, BACK_DOOR_OPEN);

						return;
					}
					else  // c_wait已经被生产者置为2（忽略），说明生产者已经生产完毕，但e_state不一定被及时置为2（满），需要进行轮询式判断
					{
loop_consume:
						bool is_consume = __sync_bool_compare_and_swap(&p_queue[current_consume_index].e_state, FULL, CONSUME);
						if (true == is_consume)
						{
							// 消费数据
							t_consume = p_queue[current_consume_index].data;
							p_queue[current_consume_index].c_wait = C_INIT;

							__AwakeProduct(current_consume_index);

							// 打开后门
							__sync_lock_test_and_set(&p_queue[current_consume_index].b_door, BACK_DOOR_OPEN);

							return;	
						}
						else  // 此种情况极少发生，一旦发生循环判断
						{
							goto loop_consume;
						}
					}
				}
			}
			else  // 已经有消费者进入，继续获取entry位置
			{
				goto loop_entry_consume;
			}
		}

	private:
		enum entry_state {EMPTY = 0, PRODUCT, FULL, CONSUME};
		enum back_door {BACK_DOOR_OPEN = 0, BACK_DOOR_CLOSE};
		enum product_wait {P_INIT = 0, P_WAIT, P_IGNORE};
		enum consume_wait {C_INIT = 0, C_WAIT, C_IGNORE};

		typedef struct 
		{
			T data;

			entry_state e_state;

			back_door b_door;

			pthread_mutex_t product_mutex;
			pthread_cond_t product_cond;
			bool product_awake_flag;  // true：表示生产者等待消费者唤醒，false：表示生产者已被消费者唤醒
			product_wait p_wait;

			pthread_mutex_t consume_mutex;
			pthread_cond_t consume_cond;
			bool consume_awake_flag;  // true：表示消费者等待生产者唤醒，false：表示消费者已被生产者唤醒
			consume_wait c_wait;
		} ENTRY;

		ENTRY *p_queue __attribute__((aligned(64)));
		unsigned int size __attribute__((aligned(64)));
		unsigned long product_index __attribute__((aligned(64)));
		unsigned long consume_index __attribute__((aligned(64)));

		inline void __AwakeConsume(unsigned long __current_product_index)
		{
			// 判断是忽略消费者还是唤醒消费者
			bool is_ignore = __sync_bool_compare_and_swap(&p_queue[__current_product_index].c_wait, C_INIT, C_IGNORE);
			if (true == is_ignore)  // 忽略消费者
			{
				__sync_lock_test_and_set(&p_queue[__current_product_index].e_state, FULL);
			}
			else  // 唤醒消费者
			{			
				p_queue[__current_product_index].e_state = FULL;

				pthread_mutex_lock(&p_queue[__current_product_index].consume_mutex);
				p_queue[__current_product_index].consume_awake_flag = true;
				pthread_cond_signal(&p_queue[__current_product_index].consume_cond);
				pthread_mutex_unlock(&p_queue[__current_product_index].consume_mutex);
			}

			return;
		}

		inline void __AwakeProduct(unsigned long __current_consume_index)
		{
			// 判断是忽略生产者还是唤醒生产者
			bool is_ignore = __sync_bool_compare_and_swap(&p_queue[__current_consume_index].p_wait, P_INIT, P_IGNORE);
			if (true == is_ignore)  // 忽略生产者
			{
				p_queue[__current_consume_index].e_state = EMPTY;
			}
			else  // 唤醒生产者
			{			
				__sync_lock_test_and_set(&p_queue[__current_consume_index].e_state, EMPTY);

				pthread_mutex_lock(&p_queue[__current_consume_index].product_mutex);
				p_queue[__current_consume_index].product_awake_flag = true;
				pthread_cond_signal(&p_queue[__current_consume_index].product_cond);
				pthread_mutex_unlock(&p_queue[__current_consume_index].product_mutex);
			}

			return;
		}
};

// 单生产者单消费者阻塞队列
template <class T>
class CasQueueOPOC
{
	public:
		CasQueueOPOC()
		{
			size = 16384;
			product_index = consume_index = 0;

			p_queue = new ENTRY [size];

			// 初始化队列
			for (int ii = 0; ii < size; ++ii)
			{
				p_queue[ii].e_state = EMPTY;

				pthread_mutex_init(&p_queue[ii].product_mutex, NULL);
				pthread_cond_init(&p_queue[ii].product_cond, NULL);
				p_queue[ii].product_awake_flag = false;
				p_queue[ii].p_wait = P_INIT;

				pthread_mutex_init(&p_queue[ii].consume_mutex, NULL);
				pthread_cond_init(&p_queue[ii].consume_cond, NULL);
				p_queue[ii].consume_awake_flag = false;
				p_queue[ii].c_wait = C_INIT;
			}
		}

		CasQueueOPOC(int queue_size)
		{
			product_index = consume_index = 0;

			size = pow(2, (ceil(log2(queue_size))));
			p_queue = new ENTRY [size];

			// 初始化队列
			for (int ii = 0; ii < size; ++ii)
			{
				p_queue[ii].e_state = EMPTY;

				pthread_mutex_init(&p_queue[ii].product_mutex, NULL);
				pthread_cond_init(&p_queue[ii].product_cond, NULL);
				p_queue[ii].product_awake_flag = false;
				p_queue[ii].p_wait = P_INIT;

				pthread_mutex_init(&p_queue[ii].consume_mutex, NULL);
				pthread_cond_init(&p_queue[ii].consume_cond, NULL);
				p_queue[ii].consume_awake_flag = false;
				p_queue[ii].c_wait = C_INIT;
			}
		}

		virtual ~CasQueueOPOC()
		{
			delete [] p_queue;
		}

		void Product(T &t_product)
		{
			unsigned long current_product_index = product_index++;
			current_product_index &= (size - 1);

			// 判断entry状态
			if (true == __sync_bool_compare_and_swap(&p_queue[current_product_index].e_state, EMPTY, PRODUCT))
			{
				// 生产数据
				p_queue[current_product_index].data = t_product;
				p_queue[current_product_index].p_wait = P_INIT;

				__AwakeConsume(current_product_index);

				return;
			}
			else  // 该entry已经有数据 或 有消费者正在消费数据
			{
				if (true == __sync_bool_compare_and_swap(&p_queue[current_product_index].p_wait, P_INIT, P_WAIT))
				{
					pthread_mutex_lock(&p_queue[current_product_index].product_mutex);
					while (!p_queue[current_product_index].product_awake_flag)
						pthread_cond_wait(&p_queue[current_product_index].product_cond, &p_queue[current_product_index].product_mutex);
					pthread_mutex_unlock(&p_queue[current_product_index].product_mutex);

					// 生产数据
					p_queue[current_product_index].data = t_product;
					p_queue[current_product_index].p_wait = P_INIT;
					p_queue[current_product_index].product_awake_flag = false;

					__AwakeConsume(current_product_index);

					return;
				}
				else  // p_wait已经被消费者置为2（忽略），说明消费者已经消费完毕，但e_state不一定被及时置为0（空），需要进行轮询式判断
				{
loop_product:
					bool is_product = __sync_bool_compare_and_swap(&p_queue[current_product_index].e_state, EMPTY, PRODUCT);
					if (true == is_product)
					{
						// 生产数据
						p_queue[current_product_index].data = t_product;
						p_queue[current_product_index].p_wait = P_INIT;

						__AwakeConsume(current_product_index);

						return;
					}
					else  // 此种情况极少发生，一旦发生循环判断
					{
						goto loop_product;
					}
				}
			}
		}

		void Consume(T &t_consume)
		{
			unsigned long current_consume_index = consume_index++;
			current_consume_index &= (size - 1);

			// 判断entry状态
			if (true == __sync_bool_compare_and_swap(&p_queue[current_consume_index].e_state, FULL, CONSUME))
			{
				// 消费数据
				t_consume = p_queue[current_consume_index].data;
				p_queue[current_consume_index].c_wait = C_INIT;

				__AwakeProduct(current_consume_index);

				return;
			}
			else  // 该entry已经没有数据 或 有生产者正在生产数据
			{
				bool is_wait = __sync_bool_compare_and_swap(&p_queue[current_consume_index].c_wait, C_INIT, C_WAIT);
				if (true == is_wait)  // 等待生产者唤醒
				{
					pthread_mutex_lock(&p_queue[current_consume_index].consume_mutex);
					while (!p_queue[current_consume_index].consume_awake_flag)
						pthread_cond_wait(&p_queue[current_consume_index].consume_cond, &p_queue[current_consume_index].consume_mutex);
					pthread_mutex_unlock(&p_queue[current_consume_index].consume_mutex);

					// 消费数据
					t_consume = p_queue[current_consume_index].data;
					p_queue[current_consume_index].c_wait = C_INIT;
					p_queue[current_consume_index].consume_awake_flag = false;

					__AwakeProduct(current_consume_index);

					return;
				}
				else  // c_wait已经被生产者置为2（忽略），说明生产者已经生产完毕，但e_state不一定被及时置为2（满），需要进行轮询式判断
				{
loop_consume:
					bool is_consume = __sync_bool_compare_and_swap(&p_queue[current_consume_index].e_state, FULL, CONSUME);
					if (true == is_consume)
					{
						// 消费数据
						t_consume = p_queue[current_consume_index].data;
						p_queue[current_consume_index].c_wait = C_INIT;

						__AwakeProduct(current_consume_index);

						return;
					}
					else  // 此种情况极少发生，一旦发生循环判断
					{
						goto loop_consume;
					}
				}
			}
		}

	private:
		enum entry_state {EMPTY = 0, PRODUCT, FULL, CONSUME};
		enum product_wait {P_INIT = 0, P_WAIT, P_IGNORE};
		enum consume_wait {C_INIT = 0, C_WAIT, C_IGNORE};

		typedef struct 
		{
			T data;

			entry_state e_state;

			pthread_mutex_t product_mutex;
			pthread_cond_t product_cond;
			bool product_awake_flag;  // true：表示生产者等待消费者唤醒，false：表示生产者已被消费者唤醒
			product_wait p_wait;

			pthread_mutex_t consume_mutex;
			pthread_cond_t consume_cond;
			bool consume_awake_flag;  // true：表示消费者等待生产者唤醒，false：表示消费者已被生产者唤醒
			consume_wait c_wait;
		} ENTRY;

		ENTRY *p_queue __attribute__((aligned(64)));
		unsigned int size __attribute__((aligned(64)));
		unsigned long product_index __attribute__((aligned(64)));
		unsigned long consume_index __attribute__((aligned(64)));

		inline void __AwakeConsume(unsigned long __current_product_index)
		{
			// 判断是忽略消费者还是唤醒消费者
			bool is_ignore = __sync_bool_compare_and_swap(&p_queue[__current_product_index].c_wait, C_INIT, C_IGNORE);
			if (true == is_ignore)  // 忽略消费者
			{
				__sync_lock_test_and_set(&p_queue[__current_product_index].e_state, FULL);
			}
			else  // 唤醒消费者
			{			
				p_queue[__current_product_index].e_state = FULL;

				pthread_mutex_lock(&p_queue[__current_product_index].consume_mutex);
				p_queue[__current_product_index].consume_awake_flag = true;
				pthread_cond_signal(&p_queue[__current_product_index].consume_cond);
				pthread_mutex_unlock(&p_queue[__current_product_index].consume_mutex);
			}

			return;
		}

		inline void __AwakeProduct(unsigned long __current_consume_index)
		{
			// 判断是忽略生产者还是唤醒生产者
			bool is_ignore = __sync_bool_compare_and_swap(&p_queue[__current_consume_index].p_wait, P_INIT, P_IGNORE);
			if (true == is_ignore)  // 忽略生产者
			{
				__sync_lock_test_and_set(&p_queue[__current_consume_index].e_state, EMPTY);
			}
			else  // 唤醒生产者
			{			
				p_queue[__current_consume_index].e_state = EMPTY;

				pthread_mutex_lock(&p_queue[__current_consume_index].product_mutex);
				p_queue[__current_consume_index].product_awake_flag = true;
				pthread_cond_signal(&p_queue[__current_consume_index].product_cond);
				pthread_mutex_unlock(&p_queue[__current_consume_index].product_mutex);
			}

			return;
		}
};

// 多生产者多消费者非阻塞队列
template <class T>
class CasQueueNoBlockMPMC
{
	public:
		CasQueueNoBlockMPMC()
		{
			size = 16384;
			product_index = consume_index = 0;

			p_queue = new ENTRY [size];

			// 初始化队列
			for (int ii = 0; ii < size; ++ii)
			{
				p_queue[ii].e_state = EMPTY;

				p_queue[ii].f_door = FRONT_DOOR_OPEN;
				p_queue[ii].b_door = BACK_DOOR_OPEN;
			}
		}

		CasQueueNoBlockMPMC(int queue_size)
		{
			product_index = consume_index = 0;

			size = pow(2, (ceil(log2(queue_size))));
			p_queue = new ENTRY [size];

			// 初始化队列
			for (int ii = 0; ii < size; ++ii)
			{
				p_queue[ii].e_state = EMPTY;

				p_queue[ii].f_door = FRONT_DOOR_OPEN;
				p_queue[ii].b_door = BACK_DOOR_OPEN;
			}
		}

		virtual ~CasQueueNoBlockMPMC()
		{
			delete [] p_queue;
		}

		bool Product(T &t_product)
		{
			unsigned long current_product_index = __sync_fetch_and_add(&product_index, 1);
			current_product_index &= (size - 1);

loop_product:
			bool is_enter = __sync_bool_compare_and_swap(&p_queue[current_product_index].f_door, FRONT_DOOR_OPEN, FRONT_DOOR_CLOSE);
			if (true == is_enter)
			{
				bool is_product = __sync_bool_compare_and_swap(&p_queue[current_product_index].e_state, EMPTY, PRODUCT);
				if (true == is_product)
				{

					p_queue[current_product_index].data = t_product;
					__sync_lock_test_and_set(&p_queue[current_product_index].e_state, FULL);
					__sync_lock_test_and_set(&p_queue[current_product_index].f_door, FRONT_DOOR_OPEN);

					return true;
				}
				else
				{
					__sync_lock_test_and_set(&p_queue[current_product_index].f_door, FRONT_DOOR_OPEN);
					return false;  // queue is full
				}
			}
			else
			{
				goto loop_product;
			}
		}

		bool Consume(T &t_consume)
		{
			unsigned long current_consume_index = __sync_fetch_and_add(&consume_index, 1);
			current_consume_index &= (size - 1);

loop_consume:
			bool is_enter = __sync_bool_compare_and_swap(&p_queue[current_consume_index].b_door, BACK_DOOR_OPEN, BACK_DOOR_CLOSE);
			if (true == is_enter)
			{
				bool is_consume = __sync_bool_compare_and_swap(&p_queue[current_consume_index].e_state, FULL, CONSUME);
				if (true == is_consume)
				{
					t_consume = p_queue[current_consume_index].data;
					__sync_lock_test_and_set(&p_queue[current_consume_index].e_state, EMPTY);
					__sync_lock_test_and_set(&p_queue[current_consume_index].b_door, BACK_DOOR_OPEN);

					return true;
				}
				else
				{
					__sync_lock_test_and_set(&p_queue[current_consume_index].b_door, BACK_DOOR_OPEN);
					return false;  // queue is enpty
				}
			}
			else
			{
				goto loop_consume;
			}

		}

	private:
		enum entry_state {EMPTY = 0, PRODUCT, FULL, CONSUME};
		enum front_door {FRONT_DOOR_OPEN = 0, FRONT_DOOR_CLOSE};
		enum back_door {BACK_DOOR_OPEN = 0, BACK_DOOR_CLOSE};

		typedef struct
		{
			T data;

			entry_state e_state;

			front_door f_door;
			back_door b_door;
		} ENTRY;

		ENTRY *p_queue __attribute__((aligned(64)));
		unsigned int size __attribute__((aligned(64)));
		unsigned long product_index __attribute__((aligned(64)));
		unsigned long consume_index __attribute__((aligned(64)));
};

// 多生产者单消费者非阻塞队列
template <class T>
class CasQueueNoBlockMPOC
{
	public:
		CasQueueNoBlockMPOC()
		{
			size = 16384;
			product_index = consume_index = 0;

			p_queue = new ENTRY [size];

			// 初始化队列
			for (int ii = 0; ii < size; ++ii)
			{
				p_queue[ii].e_state = EMPTY;

				p_queue[ii].f_door = FRONT_DOOR_OPEN;
			}
		}

		CasQueueNoBlockMPOC(int queue_size)
		{
			product_index = consume_index = 0;

			size = pow(2, (ceil(log2(queue_size))));
			p_queue = new ENTRY [size];

			// 初始化队列
			for (int ii = 0; ii < size; ++ii)
			{
				p_queue[ii].e_state = EMPTY;

				p_queue[ii].f_door = FRONT_DOOR_OPEN;
			}
		}

		virtual ~CasQueueNoBlockMPOC()
		{
			delete [] p_queue;
		}

		bool Product(T &t_product)
		{
			unsigned long current_product_index = __sync_fetch_and_add(&product_index, 1);
			current_product_index &= (size - 1);

loop_product:
			bool is_enter = __sync_bool_compare_and_swap(&p_queue[current_product_index].f_door, FRONT_DOOR_OPEN, FRONT_DOOR_CLOSE);
			if (true == is_enter)
			{
				bool is_product = __sync_bool_compare_and_swap(&p_queue[current_product_index].e_state, EMPTY, PRODUCT);
				if (true == is_product)
				{

					p_queue[current_product_index].data = t_product;
					__sync_lock_test_and_set(&p_queue[current_product_index].e_state, FULL);
					__sync_lock_test_and_set(&p_queue[current_product_index].f_door, FRONT_DOOR_OPEN);

					return true;
				}
				else
				{
					__sync_lock_test_and_set(&p_queue[current_product_index].f_door, FRONT_DOOR_OPEN);
					return false;  // queue is full
				}
			}
			else
			{
				goto loop_product;
			}
		}

		bool Consume(T &t_consume)
		{
			unsigned long current_consume_index = consume_index++;
			current_consume_index &= (size - 1);

			bool is_consume = __sync_bool_compare_and_swap(&p_queue[current_consume_index].e_state, FULL, CONSUME);
			if (true == is_consume)
			{
				t_consume = p_queue[current_consume_index].data;
				__sync_lock_test_and_set(&p_queue[current_consume_index].e_state, EMPTY);

				return true;
			}
			else
			{
				return false;  // queue is enpty
			}
		}

	private:
		enum entry_state {EMPTY = 0, PRODUCT, FULL, CONSUME};
		enum front_door {FRONT_DOOR_OPEN = 0, FRONT_DOOR_CLOSE};

		typedef struct
		{
			T data;

			entry_state e_state;

			front_door f_door;
		} ENTRY;

		ENTRY *p_queue __attribute__((aligned(64)));
		unsigned int size __attribute__((aligned(64)));
		unsigned long product_index __attribute__((aligned(64)));
		unsigned long consume_index __attribute__((aligned(64)));
};

// 单生产者多消费者非阻塞队列
template <class T>
class CasQueueNoBlockOPMC
{
	public:
		CasQueueNoBlockOPMC()
		{
			size = 16384;
			product_index = consume_index = 0;

			p_queue = new ENTRY [size];

			// 初始化队列
			for (int ii = 0; ii < size; ++ii)
			{
				p_queue[ii].e_state = EMPTY;

				p_queue[ii].b_door = BACK_DOOR_OPEN;
			}
		}

		CasQueueNoBlockOPMC(int queue_size)
		{
			product_index = consume_index = 0;

			size = pow(2, (ceil(log2(queue_size))));
			p_queue = new ENTRY [size];

			// 初始化队列
			for (int ii = 0; ii < size; ++ii)
			{
				p_queue[ii].e_state = EMPTY;

				p_queue[ii].b_door = BACK_DOOR_OPEN;
			}
		}

		virtual ~CasQueueNoBlockOPMC()
		{
			delete [] p_queue;
		}

		bool Product(T &t_product)
		{
			unsigned long current_product_index = product_index++;
			current_product_index &= (size - 1);

			bool is_product = __sync_bool_compare_and_swap(&p_queue[current_product_index].e_state, EMPTY, PRODUCT);
			if (true == is_product)
			{

				p_queue[current_product_index].data = t_product;
				__sync_lock_test_and_set(&p_queue[current_product_index].e_state, FULL);

				return true;
			}
			else
			{
				return false;  // queue is full
			}
		}

		bool Consume(T &t_consume)
		{
			unsigned long current_consume_index = __sync_fetch_and_add(&consume_index, 1);
			current_consume_index &= (size - 1);

loop_consume:
			bool is_enter = __sync_bool_compare_and_swap(&p_queue[current_consume_index].b_door, BACK_DOOR_OPEN, BACK_DOOR_CLOSE);
			if (true == is_enter)
			{
				bool is_consume = __sync_bool_compare_and_swap(&p_queue[current_consume_index].e_state, FULL, CONSUME);
				if (true == is_consume)
				{
					t_consume = p_queue[current_consume_index].data;
					__sync_lock_test_and_set(&p_queue[current_consume_index].e_state, EMPTY);
					__sync_lock_test_and_set(&p_queue[current_consume_index].b_door, BACK_DOOR_OPEN);

					return true;
				}
				else
				{
					__sync_lock_test_and_set(&p_queue[current_consume_index].b_door, BACK_DOOR_OPEN);
					return false;  // queue is enpty
				}
			}
			else
			{
				goto loop_consume;
			}

		}

	private:
		enum entry_state {EMPTY = 0, PRODUCT, FULL, CONSUME};
		enum back_door {BACK_DOOR_OPEN = 0, BACK_DOOR_CLOSE};

		typedef struct
		{
			T data;

			entry_state e_state;

			back_door b_door;
		} ENTRY;

		ENTRY *p_queue __attribute__((aligned(64)));
		unsigned int size __attribute__((aligned(64)));
		unsigned long product_index __attribute__((aligned(64)));
		unsigned long consume_index __attribute__((aligned(64)));
};

// 单生产者单消费者非阻塞队列
template <class T>
class CasQueueNoBlockOPOC
{
	public:
		CasQueueNoBlockOPOC()
		{
			size = 16384;
			product_index = consume_index = 0;

			p_queue = new ENTRY [size];

			// 初始化队列
			for (int ii = 0; ii < size; ++ii)
			{
				p_queue[ii].e_state = EMPTY;
			}
		}

		CasQueueNoBlockOPOC(int queue_size)
		{
			product_index = consume_index = 0;

			size = pow(2, (ceil(log2(queue_size))));
			p_queue = new ENTRY [size];

			// 初始化队列
			for (int ii = 0; ii < size; ++ii)
			{
				p_queue[ii].e_state = EMPTY;
			}
		}

		virtual ~CasQueueNoBlockOPOC()
		{
			delete [] p_queue;
		}

		bool Product(T &t_product)
		{
			unsigned long current_product_index = product_index++;

			current_product_index &= (size - 1);

			bool is_product = __sync_bool_compare_and_swap(&p_queue[current_product_index].e_state, EMPTY, PRODUCT);
			if (true == is_product)
			{

				p_queue[current_product_index].data = t_product;
				__sync_lock_test_and_set(&p_queue[current_product_index].e_state, FULL);

				return true;
			}
			else
			{
				return false;  // queue is full
			}
		}

		bool Consume(T &t_consume)
		{
			unsigned long current_consume_index = consume_index++;

			current_consume_index &= (size - 1);

			bool is_consume = __sync_bool_compare_and_swap(&p_queue[current_consume_index].e_state, FULL, CONSUME);
			if (true == is_consume)
			{
				t_consume = p_queue[current_consume_index].data;
				__sync_lock_test_and_set(&p_queue[current_consume_index].e_state, EMPTY);

				return true;
			}
			else
			{
				return false;  // queue is enpty
			}
		}

	private:
		enum entry_state {EMPTY = 0, PRODUCT, FULL, CONSUME};

		typedef struct
		{
			T data;

			entry_state e_state;
		} ENTRY;

		ENTRY *p_queue __attribute__((aligned(64)));
		unsigned int size __attribute__((aligned(64)));
		unsigned long product_index __attribute__((aligned(64)));
		unsigned long consume_index __attribute__((aligned(64)));
};

#endif
