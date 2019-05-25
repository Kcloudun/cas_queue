#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <sys/time.h>
#include <math.h>
#include "cas_queue.hxx"

CasQueueNoBlockOPMC<string> test_queue;

void *func_product_1(void *arg)
{
	string str_product = "hello world";

	int ii = 0;
	for (; ii < 5010000; ++ii)
	{
loop_product:
		if (false == test_queue.Product(str_product))
		{
			goto loop_product;
		}
	}
	printf("product ii = %d\n", ii);
}

void *func_consume_1(void *arg)
{
	string str_consume;
	int ii = 0;
	for (; ii < 1670000; ++ii)
	{
loop_consume:
		if (false == test_queue.Consume(str_consume))
		{
			goto loop_consume;
		}
	}
	printf("consume ii = %d\n", ii);
}

void *func_consume_2(void *arg)
{
	string str_consume;
	int ii = 0;
	for (; ii < 1670000; ++ii)
	{
loop_consume:
		if (false == test_queue.Consume(str_consume))
		{
			goto loop_consume;
		}
	}
	printf("consume ii = %d\n", ii);
}

void *func_consume_3(void *arg)
{
	string str_consume;
	int ii = 0;
	for (; ii < 1670000; ++ii)
	{
loop_consume:
		if (false == test_queue.Consume(str_consume))
		{
			goto loop_consume;
		}
	}
	printf("consume ii = %d\n", ii);
}

int main(int argc, char **argv)
{
	double time_use;
	struct timeval start;
	struct timeval end;

	gettimeofday(&start, NULL);

	pthread_t t_product_1;
	pthread_t t_consume_1, t_consume_2, t_consume_3;

	pthread_create(&t_product_1, NULL, func_product_1, NULL);
	pthread_create(&t_consume_1, NULL, func_consume_1, NULL);
	pthread_create(&t_consume_2, NULL, func_consume_2, NULL);
	pthread_create(&t_consume_3, NULL, func_consume_3, NULL);

	pthread_join(t_product_1, NULL);
	pthread_join(t_consume_1, NULL);
	pthread_join(t_consume_2, NULL);
	pthread_join(t_consume_3, NULL);

	gettimeofday(&end, NULL);

	time_use = (end.tv_sec - start.tv_sec)*1000000+(end.tv_usec-start.tv_usec);//微秒
	time_use /= 1000000;

	printf("time_use is %4.3f\n", time_use);

	return 0;
}

