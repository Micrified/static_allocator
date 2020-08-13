#if !defined(STATIC_ALLOCATOR_H)
#define STATIC_ALLOCATOR_H

/*
 *******************************************************************************
 *              (C) Copyright 2020 Delft University of Technology              *
 * Created: 14/08/2020                                                         *
 *                                                                             *
 * Programmer(s):                                                              *
 * - Charles Randolph                                                          *
 *                                                                             *
 * Description:                                                                *
 *  Stateless memory allocator. Installs metadata within provided static memor *
 *  y. This memory may be mapped memory, bss memory, or just normal heap memor *
 *  y. It is assumed that the memory itself will remain valid through the dura *
 *  tion of the class's lifetime.                                              *
 *                                                                             *
 *  The copy constructor and move assignment                                   *
 *  operator both simply copy the pointer to the static memory block. The dest *
 *  ructor has no effect. It is assumed that the user will ensure atomic acces *
 *  s to the memory if memory allocation or destruction operations are perform *
 *  ed between threads or processes.                                           *
 *                                                                             *
 *  This allocator uses a single pool of variable memory blocks, with a        *
 *  first-fit allocation scheme                                                *
 *                                                                             *
 *******************************************************************************
*/


#include <iostream>
#include <vector>

template <class T>
class Static_Allocator
{
private:

    // Structure: Allocator header block
    typedef union block_h {
        struct {
            union block_h *next;     // Next element in the linked-list
            size_t size;             // Size rounded to units of sizeof(block_h)
        } d;
        max_align_t align;           // Memory alignment element
    } block_h;

    // Structure: Allocator information
    typedef struct allocator_info_t {
        void *static_memory_map;     // Pointer to memory map
        void *free_memory_map;       // Pointer to free memory map
        size_t capacity;             // Capacity of memory map
        size_t free_size;            // Number of bytes available
        block_h *free_list;          // Linked list of memory blocks
    } allocator_info_t;

    // Pointer to Allocator information (nested within memory block)
    allocator_info_t *d_allocator_info_p;

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
    	using other          = Static_Allocator<U>;
    };

    // Operator: Move assignment
    template <class U>
    Static_Allocator &operator=(Static_Allocator<U> &&origin)
    {
        // Self-assign check
        if (&origin == this) { return *this; }

        // Release held resources 
        // None

        // Ownership transfer
        this->d_allocator_info_p = 
            reinterpret_cast<allocator_info_t *>(origin.allocator_info_p());

        // HINT: Copy the pointer values, and null the origin ones
        // HINT: In a copy you just make a new and then copy contents by value
        return *this;
    }

    // Constructor
    Static_Allocator (void *static_memory_map, size_t capacity)
    {
        // Required memory: Need space for metadata and two LL block
        size_t minimum_memory_size = sizeof(allocator_info_t) + 2 * sizeof(block_h);

        // Capacity check
        if (capacity < minimum_memory_size) {
            throw std::bad_alloc();
        }

        // Struct initialization
        d_allocator_info_p = reinterpret_cast<allocator_info_t *>(static_memory_map);

        // Set memory map pointer
        d_allocator_info_p->static_memory_map = static_memory_map;

        // Set the capacity
        d_allocator_info_p->capacity = capacity;

        // Set pointer to head of free memory map
        d_allocator_info_p->free_memory_map = reinterpret_cast<uint8_t *>(static_memory_map)
            + minimum_memory_size - (2 * sizeof(block_h));

        // Set the free size
        d_allocator_info_p->free_size = capacity - minimum_memory_size;

        // Set the free list
        d_allocator_info_p->free_list = nullptr;
    }

    // Copy constructor
    Static_Allocator (const Static_Allocator &origin)
    {
        // Simply copy the static memory pointer (not protected from race conditions)
        d_allocator_info_p = origin.allocator_info_p();
    }

    // Destructor
    ~Static_Allocator ()
    {
        // No destructor needed: state is not saved in the class instance
    }

    // Support for allocating other types
    template <class U>
    Static_Allocator (const Static_Allocator<U> &other);

    // Allocate #1: General allocation
    pointer allocate (size_type n_obj)
    {
        std::cout << "Allocate called!" << std::endl;
    	size_t n_bytes = (n_obj * sizeof(T));
    	return reinterpret_cast<pointer>(this->allocate_b(n_bytes));
    }

    // Allocate #2: Placement support
    pointer allocate (size_type n_obj, const_void_pointer hint)
    {
    	return allocate(n_obj);
    }

    // Allocate #3: Typeless allocation of n bytes
    void_pointer allocate_b (size_t n_bytes)
    {
    	block_h *last, *curr;
    	size_t const unit_size = sizeof(block_h);

        // Check: Validity of fields
        if (d_allocator_info_p == nullptr || 
            d_allocator_info_p->static_memory_map == nullptr) {
            throw std::invalid_argument("Uninitialized static memory");
        }

    	// Check: Requested byte count
    	if (n_bytes == 0) {
            throw std::invalid_argument("Cannot allocate zero bytes");
    	}

    	// Compute blocks needed (one extra block for segment header)
    	size_t n_blocks = (n_bytes + unit_size - 1) / unit_size + 1;

    	// Check: Sufficient capacity
    	if ((n_blocks * unit_size) > d_allocator_info_p->free_size) {
    		return NULL;
    	}

    	// If uninitialized: Create initial list structure
    	if ((last = d_allocator_info_p->free_list) == NULL) {
    		block_h *head = reinterpret_cast<block_h *>(
                d_allocator_info_p->free_memory_map);
    		head->d.size = 0;

    		block_h *init = head + 1;
    		init->d.size = (d_allocator_info_p->free_size) / unit_size;
    		init->d.next = (d_allocator_info_p->free_list) = last = head;

    		head->d.next = init;
    	}

    	// Find free space: Stop if wrap-around occurs
    	for (curr = last->d.next; ; last = curr, curr = curr->d.next) {

    		// Case: Enough space
    		if (curr->d.size >= n_blocks) {

    			// Case: Exactly enough
    			if (curr->d.size == n_blocks) {
    				last->d.next = curr->d.next;
    			} else {
    			// Case: More than enough
    				curr->d.size -= n_blocks;
    				curr += curr->d.size;
    				curr->d.size = n_blocks;
    			}

    			// Reassign free list head
                d_allocator_info_p->free_list = last;

    			// Update amount of free memory available
    			d_allocator_info_p->free_size -= n_blocks * unit_size;

    			return reinterpret_cast<void *>(curr + 1);
    		}

    		// Case: Insufficient. If at head, then no block found
    		if (curr == (d_allocator_info_p->free_list)) {
    			return NULL;
    		}
    	}
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
    	block_h *b, *p;
    	size_t const unit_size = sizeof(block_h);

    	// Parameter check: Is pointer valid
    	if (ptr == nullptr) {
            throw std::invalid_argument("Cannot free nullptr!");
    	}

    	// Address range of returned object
        uint8_t *obj_addr_start = reinterpret_cast<uint8_t *>(ptr);
        uint8_t *obj_addr_end   = obj_addr_start + (n_obj * sizeof(T));

        // Address of range of static memory block
        uint8_t *static_addr_start = 
            reinterpret_cast<uint8_t *>(d_allocator_info_p->static_memory_map);
        uint8_t *static_addr_end = static_addr_start + (d_allocator_info_p->capacity);
    	
        // Parameter check: Pointer address range
    	if (!(obj_addr_start > static_addr_start && 
            obj_addr_end <= static_addr_end))
    	{
            throw std::invalid_argument("Pointer originates outside valid bounds");  		
    	}

    	// Block header
    	b = (reinterpret_cast<block_h *>(ptr)) - 1;

    	// Locate insertion point
    	for (p = d_allocator_info_p->free_list; 
            !(b >= p && b < p->d.next); p = p->d.next) {

    		// Case: Block comes at end of list
    		if (p >= p->d.next && b > p) {
    			break;
    		}

    		// Case: End of list, but block first after wrap-around
    		if (p >= p->d.next && b < p->d.next) {
    			break;
    		}
    	}

    	// Check: Forward merge possible
    	if (b + b->d.size == p->d.next) {
    		b->d.size += (p->d.next)->d.size;
    		b->d.next = (p->d.next)->d.next;
    	} else {
    		b->d.next = p->d.next;
    	}

    	// Check: Backward merge possible
    	if (p + p->d.size == b) {
    		p->d.size += b->d.size;
    		p->d.next = b->d.next;
    	} else {
    		p->d.next = b;
    	}

    	// Update free-list pointer
    	d_allocator_info_p->free_list = p;

    	// Update available memory size
    	d_allocator_info_p->free_size += b->d.size * unit_size;
    }

    // Number of available bytes
    size_t free_size () const
    {
    	if (d_allocator_info_p == nullptr) {
            throw std::runtime_error("Uninitialized allocator information");
        } else {
            return d_allocator_info_p->free_size;
        }
    }

    // Whether the memory is unified
    bool unified () const
    {
        if (d_allocator_info_p == nullptr) {
            throw std::runtime_error("Uninitialized allocator information");
        }

        if (d_allocator_info_p->static_memory_map == nullptr) {
            std::cerr << __FILE__ << ":" << __LINE__ << ":" << __func__ << ": "
            "Uninitialized allocator cannot be unified (no memory)" << std::endl;
            return false;
        }

        if (d_allocator_info_p->free_list == nullptr) {
            std::cerr << __FILE__ << ":" << __LINE__ << ":" << __func__ << ": "
            "Uninitialized allocator cannot be unified (no list)" << std::endl;
            return false;
        }

        block_h *b = reinterpret_cast<block_h *>(d_allocator_info_p->free_list);

        return ((b->d.next)->d.next == b);
    }

    // Returns the allocator information
    allocator_info_t *allocator_info_p () const
    {
        return d_allocator_info_p;
    }
};



char memory_map[4096];

template <typename T>
using MyVector = std::vector<T, Static_Allocator<T>>;

int main()
{
    Static_Allocator<int> my_allocator{memory_map, 4096};

	// Create vector, but supply parameters to the template arguments
	MyVector<int> vector_1{5, my_allocator};
    MyVector<int> vector_2{3, my_allocator};

    // Vec 1
	vector_1[0] = 1;
	vector_1[1] = 2;
	vector_1[2] = 3;
	vector_1[3] = 4;
	vector_1[4] = 5;

    // Vec 2
    vector_2[0] = 6;
    vector_2[1] = 7;
    vector_2[2] = 8;

    // Print allocator pointers to make sure they are the same
    // std::cout << "Vector_1 allocator: " << vector_1.get_allocator() << std::endl;
    // std::cout << "Vector_2 allocator: " << vector_2.get_allocator() << std::endl;
    for (int i = 0; i < 5; ++i) { std::cout << "vector_1[" << i << "] = " << vector_1[i] << std::endl; }
    for (int i = 0; i < 3; ++i) { std::cout << "vector_2[" << i << "] = " << vector_2[i] << std::endl; }

	std::cout << "Free bytes remaining = " << vector_1.get_allocator().free_size() << "\n";
    std::cout << "Unified = " << my_allocator.unified() << std::endl;

    // Check for unified memory


	return 0;
}

#endif