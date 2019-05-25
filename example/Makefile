all:
	g++ -o mpmc main_mpmc.cxx -lpthread -I..
	g++ -o mpoc main_mpoc.cxx -lpthread -I..
	g++ -o opmc main_opmc.cxx -lpthread -I..
	g++ -o opoc main_opoc.cxx -lpthread -I..
	g++ -o noblock_mpmc main_noblock_mpmc.cxx -lpthread -I..
	g++ -o noblock_mpoc main_noblock_mpoc.cxx -lpthread -I..
	g++ -o noblock_opmc main_noblock_opmc.cxx -lpthread -I..
	g++ -o noblock_opoc main_noblock_opoc.cxx -lpthread -I..
clean:
	rm -f mpmc opoc mpoc opmc noblock_mpmc noblock_mpoc noblock_opmc noblock_opoc
