
class TrackingProcessor : public Processor
{
public:
	struct Object
	{
		int nr;
		uint32_t addr;
		uint32_t len;
		Object *next;
		
		Object(int n, uint32_t a, uint32_t l) : nr(n), addr(a), len(l), next(0) {}
	};
	
	Object *objects;
	Object **ref_object;
	int nr_objects;
	
	TrackingProcessor(Process *proc) : Processor(proc), objects(0), nr_objects(0) { ref_object = &objects; }
	
	void add_object(uint32_t addr, uint32_t len)
	{
		*ref_object = new Object(++nr_objects, addr, len);
		printf("%4d %08x %08x ", (*ref_object)->nr, (*ref_object)->addr, (*ref_object)->len);
		ref_object = &(*ref_object)->next;
	}
	
	void print_char(byte b)
	{
		if (' ' <= b && b < 127)
			printf("'%c' ", b);
		else
			printf("%02x ", b);
	}
	
	void print_objects()
	{
		for (Object *object = objects; object != 0; object = object->next)
		{
			printf("%4d %08x %08x ", object->nr, object->addr, object->len);
			
			bool is_string = object->len > 0;
			for (uint32_t i = 0; i < object->len - 1&& is_string; i++)
			{
				byte ch = _process->loadByte(object->addr + i);
				if (ch == '\0')
				{
					is_string = i > 0;
					break;
				}
				print_char(ch);
				is_string = ch == '\n' || (' ' <= ch && ch < 127);
			}
			if (is_string)
			{
				printf("'");
				for (uint32_t i = 0; i < object->len; i++)
				{
					byte ch = _process->loadByte(object->addr + i);
					if (ch == '\0')
					{
						break;
					}
					if (ch == '\n')
						printf("\\n");
					else
						printf("%c", ch);
				}
				printf("'");
			}
			else
			{
				for (uint32_t i = 0; i + 3 < object->len && i + 3 < 40; i += 4)
				{
					uint32_t w = _process->loadDWord(object->addr + i);
					
					bool is_object_ptr = false;
					for (Object* obj = objects; obj != 0; obj = obj->next)
						if (obj->addr == w)
						{
							printf(" ptr to object %d", obj->nr);
							is_object_ptr = true;
						}
					
					if (!is_object_ptr)
						printf(" %08x", w);
				}
			}
			printf("\n");
		}
	}			
	
};

void print_token_list(uint32_t addr)
{
}

