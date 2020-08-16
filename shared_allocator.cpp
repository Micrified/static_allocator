#if !defined(SHARED_ALLOCATOR_H)
#define SHARED_ALLOCATOR_H

/*
 *******************************************************************************
 *              (C) Copyright 2020 Delft University of Technology              *
 * Created: 15/08/2020                                                         *
 *                                                                             *
 * Programmer(s):                                                              *
 * - Charles Randolph                                                          *
 *                                                                             *
 * Description:                                                                *
 *  A shared memory allocator. This class should be instantiated only once usi *
 *  ng the parameterized constructor, and then explicity copy-constructed in o *
 *  rder to preserve state if. Allocators ought not to keep state, but the sha *
 *  red memory page used by the allocator must not be initialized twice. I kno *
 *  w this is an abuse of templates, and will try to figure out a better way t *
 *  o do it. This neeeds to be compiled with -lpthread and -lrt. Also, I recom *
 *  mend forking after the initialization of the allocator in order to ensure  *
 *  the mapped memory is positioned homogenously across all processes. Otherwi *
 *  se this could cause errors. It is a known portability problem              *
 *                                                                             *
 *******************************************************************************
*/

// C++ libraries
#include <iostream>
#include <vector>

// C libraries
extern "C" {
	#include <stdlib.h>
	#include <string.h>
	#include <unistd.h>
	#include <errno.h>
	#include <fcntl.h>
	#include <sys/stat.h>
	#include <sys/mman.h>
	#include <semaphore.h>
}

// Custom headers
#include "static_allocator.cpp"


// Max length of the shared allocator
#define MAX_SHM_MAP_NAME_SIZE        32


template <class T>
class Shared_Allocator
{
private:

	// Structure: Metadata for shared memory management
	typedef struct {
		sem_t sem;               // Access-control semaphore
		unsigned int ref_count;  // Reference count semaphore
		void *shm_map_ptr;       // Pointer to map in process space
		size_t shm_map_size;     // Size of the shared map
		char shm_map_name[MAX_SHM_MAP_NAME_SIZE + 1];   // Name of the shared map
	} shared_map_info_t;


	// Pointer: Metadata
	shared_map_info_t *d_shared_map_info_p;

	// Object: Static data allocator that actually manages the memory
	Static_Allocator<T> d_static_allocator;


	// Inline method: Get exclusive access to shared memory
	inline void take_sem ()
	{
		if (sem_wait(&(d_shared_map_info_p->sem)) == -1)
		{
			throw std::system_error(errno, std::generic_category(), "sem_wait");
		}
	}

	// Inline method: Drop exclusive access to shared memory
	inline void drop_sem ()
	{
		if (sem_post(&(d_shared_map_info_p->sem)) == -1)
		{
			throw std::system_error(errno, std::generic_category(), "sem_wait");
		}
	}

public:

    // Alias: Value types
    using value_type         = T;

	// Alias: Pointer as pointer to value type
	using pointer            = T *;

	// Alias: Pointer to const
	using const_pointer      = T const *;

	// Alias: Most general pointer
	using void_pointer       = void *;

	// Alias: Most general pointer (to const)
	using const_void_pointer = const void *;

	// Alias: Reference
	using reference          = T&;

	// Alias: Constant reference
	using const_reference    = const T&;

	// Alias: Allocation and deallocation size
	using size_type          = size_t;

    // Rebinding support: Let container construct arbitrary type allocator
    template <class U>
    struct rebind {
    	using other          = Shared_Allocator<U>;
    };

    // Operator: Move assignment
    template <class U>
    Shared_Allocator &operator=(Shared_Allocator<U> &&origin)
    {
        // Self-assign check
        if (&origin == this) { return *this; }

        // Release held resources 
        // None

        // Ownership transfer
        this->d_shared_map_info_p = origin.shared_map_info_p();
        this->d_static_allocator = origin.static_allocator();

        // Note: We don't adjust the reference count because the ownership
        // is being transferred (i.e. net reference count remains unchanged)

        // HINT: Copy the pointer values, and null the origin ones
        // HINT: In a copy you just make a new and then copy contents by value
        return *this;
    }

	// Constructor
	Shared_Allocator (char const *shared_map_name, size_t shared_map_size)
	{
		int err;              // Contains return value (usually errno)
		int shm_obj_fd = -1;  // File-descriptor for shared memory file
		void *shm_map_ptr = nullptr; // Pointer to mapped shared memory
		d_shared_map_info_p = nullptr;

		// Check: Name is appropriate length
		if (strnlen(shared_map_name, MAX_SHM_MAP_NAME_SIZE + 1) 
			>= MAX_SHM_MAP_NAME_SIZE + 1)
		{
			throw std::invalid_argument("Shared map name too long");
		}

		// Parameters: Shared Memory Object
		int shm_flags = O_CREAT | O_RDWR | O_TRUNC;  // Create/reset map
		mode_t shm_mode = S_IRUSR | S_IWUSR;         // Read/Write for user

		// Open POSIX shared memory map (init length to zero)
		if ((shm_obj_fd = shm_open(shared_map_name, shm_flags, shm_mode))
			== -1)
		{
			throw std::system_error(errno, std::generic_category(), "shm_open");
		}

		// Parameters: Shared Memory Object Properties
		size_t required_shared_map_size = shared_map_size + sizeof(shared_map_info_t);

		// Set size via ftruncate
		if ((err = ftruncate(shm_obj_fd, required_shared_map_size)) == -1)
		{
			throw std::system_error(errno, std::generic_category(), "ftruncate");
		}

		// Mapping parameters
		void *mmap_addr = nullptr;              // Let address be auto-chosen
		int mmap_prot = PROT_READ | PROT_WRITE; // Mapped memory access
		int mmap_flags = MAP_SHARED;            // Share this map between processes
		off_t mmap_offset = 0;                  // Zero offset

		// Map shared memory into process
		if ((shm_map_ptr = mmap(mmap_addr, required_shared_map_size, mmap_prot,
			mmap_flags, shm_obj_fd, mmap_offset))
			== MAP_FAILED)
		{
			throw std::system_error(0x0, std::generic_category(), "mmap");
		} else {

			// Mapping successful: Close object now
			close(shm_obj_fd);
		}

		// Parameters: Information structure
		void *free_shm_map_ptr = reinterpret_cast<uint8_t *>(shm_map_ptr) 
		    + sizeof(shared_map_info_t);

		// Install information structure
		d_shared_map_info_p = reinterpret_cast<shared_map_info_t *>(shm_map_ptr);
		d_shared_map_info_p->ref_count = 1;
		d_shared_map_info_p->shm_map_ptr = free_shm_map_ptr;
		d_shared_map_info_p->shm_map_size = shared_map_size;

		// Copy in name
		strncpy(d_shared_map_info_p->shm_map_name, shared_map_name, 
			MAX_SHM_MAP_NAME_SIZE);

		// Parameters: Unnamed semaphore
		int sem_pshared = 1;       // Share between processes, NOT threads
		int sem_init_value = 1;    // Init in an open state

		// Init unnamed semaphore
		if ((err = sem_init(&(d_shared_map_info_p->sem), sem_pshared,
			sem_init_value)) != 0)
		{
			throw std::system_error(errno, std::generic_category(), "sem_init");
		}

		// Setup the static allocator
		Static_Allocator<T> static_allocator{d_shared_map_info_p->shm_map_ptr, 
			d_shared_map_info_p->shm_map_size};

		// Assign (move operator of class will simply transfer ownership)
		d_static_allocator = static_allocator;
	}

    // Copy constructor
    Shared_Allocator (const Shared_Allocator &origin)
    {
    	// Simply copy info for shared memory and allocator
    	d_shared_map_info_p = origin.shared_map_info_p();
    	d_static_allocator = origin.static_allocator();

    	// Update the reference count
    	take_sem();
    	d_shared_map_info_p->ref_count++;
    	drop_sem();
    }

    // Support for allocating other types
    template <class U>
    Shared_Allocator (const Shared_Allocator<U> &other);

	// Destructor: We cannot recover from exceptions here - termianate on throw
	~Shared_Allocator () noexcept(false)
	{
		bool destroy = false;

		// Check: update reference count
		take_sem();
		d_shared_map_info_p->ref_count -= 1;
		destroy = (d_shared_map_info_p->ref_count == 0);
		drop_sem();

		// Optionally: Remove infrastructure if no references left
		if (destroy)
		{
			int err;
			std::cout << "[" << getpid() << "] "
			    << "~Shared_Allocator(destroy=true)" << std::endl;
			// #1: delete semaphore
			if (sem_destroy(&(d_shared_map_info_p->sem)) == -1)
			{
				throw std::system_error(errno, std::generic_category(), 
					"sem_destroy");
			}

			// #2: copy the name + size out so we can unlink after unmap
			char shm_map_name[MAX_SHM_MAP_NAME_SIZE + 1];
			memcpy(shm_map_name, d_shared_map_info_p->shm_map_name, 
				MAX_SHM_MAP_NAME_SIZE + 1);
			size_t shm_map_size = d_shared_map_info_p->shm_map_size + 
				sizeof(shared_map_info_t);

			// #3: unmap the memory page
			if ((err = munmap(d_shared_map_info_p, shm_map_size)) == -1)
			{
				throw std::system_error(errno, std::generic_category(), 
					"munmap");
			}


			// #4: Unlink the shared object
			if ((err = shm_unlink(shm_map_name)) == -1)
			{
				throw std::system_error(errno, std::generic_category(), 
					"shm_unlink");
			}
		} else {
			std::cout << "[" << getpid() << "] "
			    << "~Shared_Allocator(destroy=false)" << std::endl;
		}
	}

	// Allocate #1: General allocation
	pointer allocate (size_type n_obj)
	{
		return d_static_allocator.allocate(n_obj);
	}

	// Allocate #2: Placement support
	pointer allocate (size_type n_obj, const_void_pointer hint)
	{
		return allocate(n_obj);
	}

	// Allocate #3: Typeless allocation of n bytes
	void_pointer allocate_b (size_t n_bytes)
	{
		return d_static_allocator.allocate_b(n_bytes);
	}

    // Convert a reference to a pointer
    pointer address (reference r) const
    {
    	return static_cast<pointer>(std::addressof(r));
    }

    // Converts a reference to a const pointer
    const_pointer address (const_reference r) const
    {
    	return static_cast<const_pointer>(std::addressof(r));
    }

    // Deallocate
    void deallocate (pointer ptr, size_type n_obj)
    {
    	return d_static_allocator.deallocate(ptr, n_obj);
    }

    // Available memory to allocate
    size_t free_size () const
    {
    	return d_static_allocator.free_size();
    }

    bool unified () const
    {
    	return d_static_allocator.unified();
    }

	shared_map_info_t *shared_map_info_p () const
	{
		return this->d_shared_map_info_p;
	}

	Static_Allocator<T> static_allocator () const
	{
		return this->d_static_allocator;
	}
};


int main ()
{
	char const *shared_map_name = "rosmem";
	size_t shared_map_size = 4096;

	// Get process PID
	int pid = getpid();

	// Make the shared allocator
	Shared_Allocator<int> my_allocator{shared_map_name, shared_map_size};

	// Display the number of bytes
	std::cout << "[" << pid << "] "
			  << "Bytes (asked = " << shared_map_size
	          << ", free = "  << my_allocator.free_size()
	          << ")" << std::endl;

	// Allocate a vector
	{
		std::vector<int, Shared_Allocator<int>> my_vector{6, my_allocator};

		// Zero the vector
		std::fill(my_vector.begin(), my_vector.end(), 0);

		// Fork here
		if (fork() == 0) {
			pid = getpid();
			my_vector[3] = 4;
			my_vector[4] = 5;
			my_vector[5] = 6;
		} else {
			my_vector[0] = 1;
			my_vector[1] = 2;
			my_vector[2] = 3;
		}

		// Both sleep for some time to allow each other to catch up
		sleep(1);
		// TODO: Need sync here

		// Print some output
		std::cout << "[" << pid << "] "
	        << "Sum of vector = " << 
		    (my_vector[0] + my_vector[1] + my_vector[2] + 
		     my_vector[3] + my_vector[4] + my_vector[5]) << std::endl;

		// TODO: Need some kind of barrier here for deallocation control
    }

	// Memory check
	std::cout << "[" << getpid() << "] " <<
	    "Bytes (allocated = " << my_allocator.free_size() << ")" << std::endl;
	std::cout << "[" << getpid() << "] " <<
	    "Unified = " << my_allocator.unified() << std::endl;

	return 0;
}

#endif