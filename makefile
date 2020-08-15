shared_allocator: shared_allocator.cpp static_allocator.cpp
	g++ -o $@ $^ -lpthread -lrt

clean: shared_allocator
	rm $^