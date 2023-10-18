/*
	http://ref.x86asm.net/geek.html
	https://www.felixcloutier.com/x86/
*/
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstdint>
#include <string.h>
#include <stdarg.h>

char *copystr(const char *v)
{
	char *result = (char*)malloc(strlen(v) + 1);
	strcpy(result, v);
	return result;
}

#define MAX_NR_MESSAGES 20
#define MAX_MESSAGE_LEN 200
char messages[MAX_NR_MESSAGES][MAX_MESSAGE_LEN];
int message_nr = 0;

void trace(const char* format, ...)
{
	va_list argp;
	va_start(argp, format);
#if 0
	vfprintf(stdout, format, argp);
#else
	vsnprintf(messages[message_nr], MAX_MESSAGE_LEN, format, argp);
	messages[message_nr][MAX_MESSAGE_LEN-1] = '\0';
	message_nr = (message_nr + 1) % MAX_NR_MESSAGES;
#endif
	va_end(argp);
}

void print_trace(FILE *f)
{
	printf("---\n");
	for (int i = message_nr; i < MAX_NR_MESSAGES; i++)
		printf("%s", messages[i]);
	for (int i = 0; i < message_nr; i++)
		printf("%s", messages[i]);
	printf("---\n");
}


int indent_depth = 0;
void indent(FILE *fout) { fprintf(fout, "%*.*s", indent_depth, indent_depth, ""); }



typedef unsigned char byte;

class Process;
class Usage;

class File
{
public:
	char *name;
	char *path;

	int nr;
	
	bool is_source;
	bool produced;
	Process *produced_by;
	bool exec;
	bool used_as_input;
	bool removed;
	
	int fh; // -1 => closed

	Usage *usages;

	File *next;
	
	File(const char *fn, int _nr) : path(0), nr(_nr), is_source(false), produced(false), produced_by(0), exec(false), used_as_input(0), removed(0), fh(-1), usages(0), next(0)
	{
		name = copystr(fn);
	}
};

File *files = 0;

File *getFile(const char *name)
{
	static int nr = 1;
	File **ref_file = &files;
	for (; *ref_file != 0; ref_file = &(*ref_file)->next)
		if (strcmp((*ref_file)->name, name) == 0)
			return *ref_file;
	File *file = new File(name, nr++);
	*ref_file = file;
	ref_file = &(*ref_file)->next;
	return file;
}


class Process
{
public:
	const char *name;
	int argc;
	char **argv;
	char **env;
	
	byte **memory[256];
	
	uint32_t pc;
	uint32_t sp;
	uint32_t end_code;
	uint32_t brk;
	Process *parent;
	
	Usage *uses;
	
	Process *next;
	
	Process(Process *_parent = 0) : parent(_parent), uses(0), next(0)
	{
		name = "";
		
		for (int i = 0; i < 256; i++)
			memory[i] = 0;
		sp = 0xFFFFFFFFL;
		if (parent != 0)
		{
			pc = parent->pc;
			sp = parent->sp;
			end_code = parent->end_code;
			brk = parent->brk;
			for (int i = 0; i < 256; i++)
				if (parent->memory[i] != 0)
				{
					memory[i] = (byte**)malloc(0x100 * sizeof(byte *));
					for (int j = 0; j < 256; j++)
						if (parent->memory[i][j] == 0)
							memory[i][j] = 0;
						else
						{
							memory[i][j] = (byte*)malloc(0x10000 * sizeof(byte));
							for (int k = 0; k < 0x10000; k++)
								memory[i][j][k] = parent->memory[i][j][k];
						}
				}
		}
	}
	~Process()
	{
		for (int i = 0; i < 256; i++)
			if (memory[i] != 0)
			{
				for (int j = 0; j < 256; j++)
					free(memory[i][j]);
				free(memory[i]);
			}
	}

	void init(int argc, char *argv[], char *env[])
	{
		name = argv[0];
		this->argc = argc;
		this->argv = argv;
		this->env = env;
		
		uint32_t p = 4;
		
		int nr_env = 0;
		while (env[nr_env] != 0)
			nr_env++;
		
		push(0);	
		for (int i = nr_env - 1; i >= 0; i--)
		{
			push(p);
			for (int j = 0; ; j++)
			{
				storeByte(p++, env[i][j]);
				if (env[i][j] == 0)
					break;
			}
		}
		
		push(0);
		for (int i = argc - 1; i >= 0; i--)
		{
			push(p);
			for (int j = 0; ; j++)
			{
				storeByte(p++, argv[i][j]);
				if (argv[i][j] == 0)
					break;
			}
		}
		push(argc);
	}
	
	void finish();
	
	byte loadByte(uint32_t address)
	{
		byte hi = (byte)((address >> 24) & 0xff);
		if (memory[hi] == 0)
			return 0;
		byte hi2 = (byte)((address >> 16) & 0xff);
		if (memory[hi][hi2] == 0)
			return 0;
		return memory[hi][hi2][(address & 0xffff)];
	}

	void storeByte(uint32_t address, byte value)
	{
		byte hi = (byte)((address >> 24) & 0xff);
		if (memory[hi] == 0)
		{
			memory[hi] = (byte**)malloc(0x100 * sizeof(byte *));
			for (int i = 0; i < 256; i++)
				memory[hi][i] = 0;
		}
		byte hi2 = (byte)((address >> 16) & 0xff);
		if (memory[hi][hi2] == 0)
			memory[hi][hi2] = (byte*)malloc(0x10000 * sizeof(byte));
		memory[hi][hi2][(address & 0xffff)] = value;
	}
	
	uint32_t loadDWord(uint32_t address)
	{
		uint32_t value = 0;
		for (int i = 0; i < 4; i++)
			value |= (uint32_t)loadByte(address + i) << (i * 8);
		return value;
	}
	
	void storeDWord(uint32_t address, uint32_t value)
	{
		for (int i = 0; i < 4; i++)
		{
			storeByte(address + i, value & 0xff);
			value = value >> 8;
		}
	}

	void push(uint32_t value)
	{
		sp -= 4;
		trace("push %08x to %08x\n", value, sp);
		storeByte(sp    , (byte)value);
		storeByte(sp + 1, (byte)(value >> 8));
		storeByte(sp + 2, (byte)(value >> 16));
		storeByte(sp + 3, (byte)(value >> 24));
	}
	
	uint32_t pop()
	{
		if (sp == 0xFFFFFFFFL)
		{
			fprintf(stderr, "Stack underflow\n");
			exit(-1);
		}
		uint32_t value = 0;
		value |= ((uint32_t)loadByte(sp));
		value |= ((uint32_t)loadByte(sp + 1)) << 8;
		value |= ((uint32_t)loadByte(sp + 2)) << 16;
		value |= ((uint32_t)loadByte(sp + 3)) << 24;
		trace("pop %08x from %08x\n", value, sp);
		sp += 4;
		
		return value;
	}
};

Process *processes = 0;

Process *newProcess(Process *parent)
{
	static Process **ref_next_process = &processes;
	Process *process = new Process(parent);
	*ref_next_process = process;
	ref_next_process = &process->next;
	return process;
}

class Usage
{
public:
	bool as_input;
	bool as_output;
	bool as_exec;
	bool as_removed;
	
	File *file;
	Process *process;
	
	Usage *next_use;
	Usage *next_usage;
	
	Usage (File *_file, Process *_process) : as_input(false), as_output(false), as_exec(false), as_removed(false), file(_file), process(_process), next_use(0), next_usage(0)
	{
		Usage **ref_use = &process->uses;
		while (*ref_use != 0) ref_use = &(*ref_use)->next_use;
		*ref_use = this;
		Usage **ref_usage = &file->usages;
		while (*ref_usage != 0) ref_usage = &(*ref_usage)->next_usage;
		*ref_usage = this;
	}
	
	void is_input(FILE *fout)
	{
		as_input = true;
		file->used_as_input = true;
		if (fout != 0) { indent(fout); fprintf(fout, "%s file: %s\n", /*file->exists ? "Existing" :*/ file->produced ? "Produced" : "Input", file->name); }
	}
	void is_output(FILE *fout)
	{
		as_output = true;
		if (fout != 0) { indent(fout); fprintf(fout, "Output file: %s\n", file->name); }
		file->produced = true;
		file->produced_by = process;
		file->removed = false;
	}
	void is_exec(FILE *fout)
	{
		as_exec = true;
		if (fout != 0)
		{
			indent(fout); fprintf(fout, "Exec file: %s", file->name);
			//if (!file->exists && !file->produced)
			//	fprintf(fout, " Not existing nor produced");
			//if (file->exists)
			//	fprintf(fout, " Existing");
			if (file->produced)
				fprintf(fout, " Produced");
			fprintf(fout, "\n");
		}
	}
	void is_removed(FILE *fout)
	{
		as_removed = true;
		file->removed = true;
		indent(fout); fprintf(fout, "Deleted file: %s\n", file->name);
	}
};

void Process::finish()
{
	printf("Close process\n");
	for (Usage *use = uses; use != 0; use = use->next_use)
		if (use->file->fh >= 0)
		{
			printf(" Close file %s\n", use->file->path);
			close(use->file->fh);
			use->file->fh = -1;
		}
}



char *root_dir = 0;
char *name_in_root(const char *name)
{
	static char fullname[500];
	strcpy(fullname, root_dir);
	if (name[0] == '.' && name[1] == '/')
		name += 2;
	strcat(fullname, name);
	return fullname;
}

class ProgramFile
{
public:
	const char *fullname;
	byte *data = 0;
	uint32_t length;
	
	ProgramFile() : data(0), length(0) {}
	bool open(const char *name)
	{
		FILE *f = fopen(name, "r");
		if (f == 0)
		{
			return false;
		}
		int fh = fileno(f);
		length = lseek(fh, 0L, SEEK_END);
		lseek(fh, 0L, SEEK_SET);
		data = new byte[length];
		length = read(fh, data, length);
		printf("Length %08x\n", length);
		for (int i = 0; i < length; i++)
			trace("%02X ", data[i]);
		trace("\n");
		fclose(f);
		return true;
	}
	
	unsigned short readShort(uint32_t &i)
	{
		unsigned short result = 0;
		result |= (unsigned short)data[i++];
		result |= (unsigned short)data[i++] << 8;
		return result;
	}
	
	uint32_t readLong(uint32_t &i)
	{
		uint32_t result = 0;
		result |= (uint32_t)data[i++];
		result |= (uint32_t)data[i++] << 8;
		result |= (uint32_t)data[i++] << 16;
		result |= (uint32_t)data[i++] << 24;
		trace("readLong %08x %08x\n", i - 4, result);
		return result;
	}
};

bool loadELF(ProgramFile *file, Process *process)
{
	process->name = file->fullname;
	
	byte signature[24] = { 
		0x7F, 0x45, 0x4C, 0x46,
		0x01,
		0x01,
		0x01,
		0x03,
		0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x02, 0x00, 
		0x03, 0x00,
		0x01, 0x00, 0x00, 0x00
	};
	uint32_t i = 0;
	for (; i < 24; i++)
		if (signature[i] != file->data[i])
		{
			fprintf(stderr, "ELF signature %2d %02X %02X\n", i, signature[i], file->data[i]);
			return false;
		}
	uint32_t pc = file->readLong(i);
	process->pc = pc;
	uint32_t phoff = file->readLong(i);
	if (phoff != 0x34)
	{
		fprintf(stderr, "Program header = %08x\n", phoff);
		return false;
	}
	uint32_t section_header = file->readLong(i);
	if (section_header != 0)
	{
		fprintf(stderr, "Section header = %08x\n", section_header);
		return false;
	}
	if (file->readLong(i) != 0)
	{
		fprintf(stderr, "e_flags = %08x\n", section_header);
		return false;
	}
	unsigned short ehsize = file->readShort(i);
	if (ehsize != 0x34)
	{
		fprintf(stderr, "ehsize = %04x\n", ehsize);
		return false;
	}
	unsigned short phentsize = file->readShort(i);
	if (phentsize != 0x20)
	{
		fprintf(stderr, "phentsize = %04x\n", phentsize);
		return false;
	}
	unsigned short phnum = file->readShort(i);
	if (phnum != 1)
	{
		fprintf(stderr, "phnum = %04x\n", phnum);
		return false;
	}
	unsigned short shentsize = file->readShort(i);
	unsigned short shnum = file->readShort(i);
	unsigned short shstrndx = file->readShort(i);

	uint32_t from_file = ehsize + phentsize * phnum;
	
	for (int ih = 0; ih < phnum; ih++)
	{
		uint32_t ph_type = file->readLong(phoff);
		uint32_t ph_offset = file->readLong(phoff);
		uint32_t ph_vaddr = file->readLong(phoff);
		uint32_t ph_physaddr = file->readLong(phoff);
		uint32_t ph_filesz = file->readLong(phoff);
		uint32_t ph_memsz = file->readLong(phoff);
		uint32_t ph_flags = file->readLong(phoff);
		uint32_t ph_align = file->readLong(phoff);
		
		if (ph_type != 1)
		{
			fprintf(stderr, "ph_type = %08x\n", ph_type);
			return false;
		}
		if (ph_offset != 0)
		{
			fprintf(stderr, "ph_offset = %08x\n", ph_offset);
			return false;
		}
		if (ph_vaddr != ph_physaddr)
		{
			fprintf(stderr, "ph_vaddr %08x %08x\n", ph_vaddr, ph_physaddr);
			return false;
		}
		if (ph_filesz != ph_memsz)
		{
			fprintf(stderr, "ph_vaddr %08x %08x\n", ph_filesz, ph_memsz);
			return false;
		}
		if (ph_flags != 7)
		{
			fprintf(stderr, "ph_flags = %08x\n", ph_flags);
			return false;
		}
		if (ph_align != 1)
		{
			fprintf(stderr, "ph_align = %08x\n", ph_align);
			return false;
		}
		
		uint32_t to_mem = ph_vaddr + from_file;
		for (uint32_t j = 0; j < ph_filesz; j++)
		{
			//printf("Load byte from %08x: %02x ", from_file, file->data[from_file]);
			process->storeByte(to_mem, file->data[from_file]);
			//printf("Stored at %08x: %02x\n", to_mem, process->loadByte(to_mem)); 
			from_file++;
			to_mem++;
		}
		process->end_code = to_mem;
		process->brk = (to_mem + 0xfff) & 0xfffff000;
	}
	
	return true;
}

class Processor
{
public:
	Processor(Process *process) : _process(process) {}
	
	void run()
	{
		_pc = _process->pc;
		for (;;)
		{
			byte opcode = getPC();
			
			switch (opcode)
			{
				case 0x01:
					opcode = getPC();
					switch (opcode)
					{
						case 0xC3:
							_ebx += _eax;
							trace(" add_ebx,eax %08x\n", _eax);
							break;
							
						case 0xF8:
							_eax += _edi;
							trace(" add_eax,edi %08x\n", _eax);
							break;
							
						default:
							unknownOpcode();
							return;
					}
					break;
				
				case 0x05:
					{
						uint32_t offset = getLongPC();
						_eax += offset;
						trace(" add_eax %08x: %08x\n", offset, _eax);
					}
					break;
					
				case 0x0F:
					opcode = getPC();
					switch (opcode)
					{
						case 0x84:
							{
								uint32_t offset = getLongPC();
								trace(" je\n");
								if (_flags == 0)
								{
									_pc += offset;
								}
							}
							break;
							
						case 0x85:
							{
								uint32_t offset = getLongPC();
								trace(" jne\n");
								if (_flags != 0)
								{
									_pc += offset;
								}
							}
							break;
							
						case 0x8C:
							{
								uint32_t offset = getLongPC();
								trace(" jl\n");
								if (_flags < 0)
								{
									_pc += offset;
								}
							}
							break;
							
						case 0xB6: // https://www.felixcloutier.com/x86/movzx
							opcode = getPC();
							switch (opcode)
							{
								case 0xC0:
									_eax = _eax & 0xFF;
									trace(" movzx _eax: %08x\n", _eax);
									break;
									
								default:
									unknownOpcode();
									return;
							}
							break;
							
						default:
							unknownOpcode();
							return;
					}
					break;
				
				case 0x2C:
					opcode = getPC();
					trace(" sub_al, %d\n", opcode);
					set_al((byte)(_eax & 0xFF) - opcode);
					break;
					 
				case 0x31:
					opcode = getPC();
					switch(opcode)
					{
						case 0xC0:
							_eax = 0L;
							trace(" xor_eax,eax\n");
							break;
						
						case 0xC9:
							_ecx = 0L;
							trace(" xor_ecx,ecx\n");
							break;
						
						case 0xD2:
							_edx = 0L;
							trace(" xor_edx,edx\n");
							break;

						case 0xDB:
							_ebx = 0L;
							trace(" xor_ebx,ebx\n");
							break;

						case 0xED:
							_ebp = 0L;
							trace(" xor_ebp,ebp\n");
							break;

						case 0xFF:
							_edi = 0L;
							trace(" xor_edi,edi\n");
							break;

						default:
							unknownOpcode();
							return;
					}
					break;
				
				case 0x39:
					opcode = getPC();
					switch(opcode)
					{
						case 0xCB:
							_flags = _ebx - _ecx;
							trace(" cmp_ebx,ecx\n");
							break;

						default:
							unknownOpcode();
							return;
					}
					break;
						
				case 0x3C:
					opcode = getPC();
					_flags = (byte)_eax - opcode;
					trace(" cmp_al, %d = %08x\n", opcode, _flags);
					break;
					
				case 0x4D:
					_ebp--;
					break;
					
				case 0x50: _process->push(_eax); trace(" push_eax\n"); break;
				case 0x51: _process->push(_ecx); trace(" push_ecx\n"); break;
				case 0x52: _process->push(_edx); trace(" push_edx\n"); break;
				case 0x53: _process->push(_ebx); trace(" push_ebx\n"); break;
					
				case 0x58:
					_eax = _process->pop();
					trace(" pop_eax: %08x\n", _eax);
					break;
					
				case 0x59:
					_ecx = _process->pop();
					trace(" pop_ecx: %08x\n", _ecx);
					break;
					
				case 0x5A:
					_edx = _process->pop();
					trace(" pop_edx: %08x\n", _ebx);
					break;
					
				case 0x5B:
					_ebx = _process->pop();
					trace(" pop_ebx: %08x\n", _ebx);
					break;
					
				case 0x5D:
					_ebp = _process->pop();
					trace(" pop_eax: %08x\n", _eax);
					break;
					
				case 0x6A:
					opcode = getPC();
					_process->push((uint32_t)(char)opcode);
					trace(" push %02x\n", opcode);
					break;
				
				case 0x66:
					opcode = getPC();
					switch (opcode)
					{
						case 0xB9:
							set_cx(getShortPC());
							break;
							
						case 0xBA:
							set_dx(getShortPC());
							break;
							
						default:
							unknownOpcode();
							return;
					}
					break;
				
				case 0x74:
					opcode = getPC();
					trace(" je %02X\n", opcode);
					if (_flags == 0)
					{
						_pc = _pc + opcode - (opcode >= 0x80 ? 0x100 : 0);
						trace("  => jump to %08x\n", _pc);
					}
					break;
				
				case 0x75:
					opcode = getPC();
					trace(" jne %02X\n", opcode);
					if (_flags != 0)
					{
						_pc = _pc + opcode - (opcode >= 0x80 ? 0x100 : 0);
						trace("  => jump to %08x\n", _pc);
					}
					break;
				
				case 0x7C:
					opcode = getPC();
					trace(" jl %02X  flags = %d\n", opcode, _flags);
					if (_flags < 0)
					{
						_pc = _pc + opcode - (opcode >= 0x80 ? 0x100 : 0);
						trace("  => jump to %08x\n", _pc);
					}
					break;
				
				case 0x7D:
					opcode = getPC();
					trace(" jge %02X\n", opcode);
					if (_flags >= 0)
					{
						_pc = _pc + opcode - (opcode >= 0x80 ? 0x100 : 0);
						trace("  => jump to %08x\n", _pc);
					}
					break;
				
				case 0x7E:
					opcode = getPC();
					trace(" jle %02X\n", opcode);
					if (_flags <= 0)
					{
						_pc = _pc + opcode - (opcode >= 0x80 ? 0x100 : 0);
						trace("  => jump to %08x\n", _pc);
					}
					break;
				
				case 0x83:
					opcode = getPC();
					switch (opcode)
					{
						case 0xC1:
							opcode = getPC();
							_ecx += opcode;
							trace(" add_ecx %02x\n", opcode);
							break;
							
						case 0xC3:
							opcode = getPC();
							_ebx += opcode;
							trace(" add_ecx %02x\n", opcode);
							break;
						
						case 0xC7:
							opcode = getPC();
							_edi += opcode;
							trace(" add_ecx %02x\n", opcode);
							break;
						
						case 0xE8: // https://www.felixcloutier.com/x86/sub
							opcode = getPC();
							_eax -= opcode;
							trace(" sub _eax %02x: %08x\n", opcode, _eax);
							break;
								
						case 0xF8: // https://www.felixcloutier.com/x86/cmp
							opcode = getPC();
							_flags = (_eax & 0xFF) - opcode;
							trace(" cmp _eax %02x: %08x\n", opcode, _flags);
							break;
								
						case 0xFD: // https://www.felixcloutier.com/x86/cmp
							opcode = getPC();
							_flags = _ebp - opcode;
							trace(" cmp _ebp %02x: %08x\n", opcode, _flags);
							break;
								
						default:
							unknownOpcode();
							return;
					}
					break;
				
				case 0x85:
					opcode = getPC();
					switch (opcode)
					{
						case 0xC0:
							_flags = (int32_t)_eax;
							trace(" test_eax,eax %08x %d\n", _eax, _flags);
							break;
							
						case 0xDB:
							_flags = (int32_t)_ebx;
							trace(" test_ebx,ebx %08x %d\n", _ebx, _flags);
							break;
							
						case 0xED:
							_flags = (int32_t)_ebp;
							trace(" test_ebp,ebp %08x %d\n", _ebp, _flags);
							break;
							
						default:
							unknownOpcode();
							return;
					}
					break;
				
				case 0x88:
					opcode = getPC();
					switch (opcode)
					{
						case 0x01:
						{
							_process->storeDWord(_ecx, (byte)_eax);
							trace(" mov_[ecx:%08x],al %02x\n", _ecx, (byte)_eax);
						}
						break;
					
						default:
							unknownOpcode();
							return;
					}
					break;

				case 0x89:
					opcode = getPC();
					switch (opcode)
					{
						case 0x01:
							_process->storeDWord(_ecx, _eax);
							trace(" mov_[ecx:%08x],eax %08x\n", _ecx, _eax);
							break;
						
						case 0x1D:
							{
								uint32_t addr = getLongPC();
								_process->storeDWord(addr, _ebx);
								trace(" mov_ebx: %08x to memory[%08x]\n", _ebx, addr);
							}
							break;
						
						case 0x38:
							_process->storeDWord(_eax, _edi);
							trace(" mov_edi: %08x to memory[eax:%08x]\n", _edi, _eax);
							break;
						
						case 0xC1: _ecx = _eax; trace(" mov_ecx,eax = %08x\n", _ecx); break;
						case 0xC2: _edx = _eax; trace(" mov_edx,eax = %08x\n", _edx); break;
						case 0xC3: _ebx = _eax; trace(" mov_ebx,eax = %08x\n", _ebx); break;
						case 0xC6: _esi = _eax; trace(" mov_esi,eax = %08x\n", _esi); break;
						case 0xC7: _edi = _eax; trace(" mov_edi,eax = %08x\n", _edi); break;
						case 0xD3: _ebx = _edx; trace(" mov_edx,eax = %08x\n", _edx); break;
						case 0xD8: _eax = _ebx; trace(" mov_eax,ebx = %08x\n", _eax); break;
						case 0xD9: _ecx = _ebx; trace(" mov_ecx,ebx = %08x\n", _ecx); break;
						case 0xDA: _edx = _ebx; trace(" mov_edx,ebx = %08x\n", _edx); break;
						case 0xE1: _ecx = _process->sp; trace(" mov_ecx,esp = %08x\n", _ecx); break;
						case 0xE5: _ebp = _process->sp; trace(" mov_ebp,esp = %08x\n", _ecx); break;
						case 0xEA: _edx = _ebp; trace(" mov_edx,ebp = %08x\n", _edx); break;
						case 0xF3: _ebx = _esi; trace(" mov_ebx,esi = %08x\n", _ebx); break;
							
						default:
							unknownOpcode();
							return;
					}
					break;
			
				case 0x8A:
					opcode = getPC();
					switch (opcode)
					{
						case 0x03:
							set_al(_process->loadByte(_ebx));
							trace(" mov_al,[ebx:%08x] %08x\n", _ebx, _eax);
							break; 
							
						default:
							unknownOpcode();
							return;
					}
					break;
				
				case 0x8B:
					opcode = getPC();
					switch (opcode)
					{
						case 0x03:
							_eax = _process->loadDWord(_ebx);
							trace(" mov_eax,[ebx:%08x] %08x\n", _ebx, _eax);
							break; 
						
						case 0x1B:
							_ebx = _process->loadDWord(_ebx);
							trace(" mov_ebs,[ebx] %08x\n", _ebx);
						break;
					
						case 0x1D:
						{
							uint32_t addr = getLongPC();
							_ebx = _process->loadDWord(addr);
							trace(" mov_ebx: %08x from memory[%08x]\n", _ebx, addr);
						}
						break;
							
						default:
							unknownOpcode();
							return;
					}
					break;
				
				case 0x8D:
					opcode = getPC();
					switch (opcode)
					{
						case 0x0C:
							opcode = getPC();
							switch (opcode)
							{
								case 0x24:
									_ecx = _process->sp;
									trace(" lea_ecx,[esp] %08x\n", _ecx);
									break;
									
								default:
									unknownOpcode();
									return;
							}
							break;

						default:
							unknownOpcode();
							return;
					}
					break;
					
				case 0xA0:
					{
						uint32_t addr = getLongPC();
						set_al(_process->loadByte(addr));
						trace(" mov_al: %08x from memory[%08x]\n", _eax, addr);
					}
					break;
						
				case 0xA1:
					{
						uint32_t addr = getLongPC();
						_eax = _process->loadDWord(addr);
						trace(" mov_eax: %08x from memory[%08x]\n", _eax, addr);
					}
					break;
						
				case 0xA3:
					{
						uint32_t addr = getLongPC();
						_process->storeDWord(addr, _eax);
						trace(" mov_eax: %08x to memory[%08x]\n", _eax, addr);
					}
					break;
				
				case 0xB8: _eax = getLongPC(); trace(" mov_eax, %08x\n", _eax); break;
				case 0xB9: _ecx = getLongPC(); trace(" mov_ecx, %08x\n", _ecx); break;
				case 0xBA: _edx = getLongPC(); trace(" mov_edx, %08x\n", _edx); break;
				case 0xBB: _ebx = getLongPC(); trace(" mov_ebx, %08x\n", _ebx); break;
				case 0xBD: _ebp = getLongPC(); trace(" mov_ebp, %08x\n", _ebp); break;
				case 0xBE: _esi = getLongPC(); trace(" mov_esi, %08x\n", _esi); break;
				case 0xBF: _edi = getLongPC(); trace(" mov_edi, %08x\n", _edi); break;

				case 0xC1:
					opcode = getPC();
					switch (opcode)
					{
						case 0xE0:
							opcode = getPC();
							_eax = _eax << opcode;
							trace(" shl_edi, %d %08x\n", opcode, _edi);
							break;
							
						case 0xE7:
							opcode = getPC();
							_edi = _edi << opcode;
							trace(" shl_edi, %d %08x\n", opcode, _edi);
							break;
							
						default:
							unknownOpcode();
							return;
					}
					break;
					
				case 0xC3:
					_pc = _process->pop();
					trace(" ret to %02x\n", _pc);
					break;
					
				case 0xCD:
					opcode = getPC();
					trace(" int %02x\n", opcode);
					switch (opcode)
					{
						case 0x80:
							switch (_eax)
							{
								case 0x01:
									// Exit
									if (_process->parent != 0)
									{
										_process->finish();
										_process = _process->parent;
										_pc = _process->pc;
										_eax = 1;
									}
									else
										return;
									break;

								case 0x02:
									int_fork();
									break;
																		
								case 0x03:
									int_read();
									break;
								
								case 0x04:
									int_write();
									break;
								
								case 0x05:
									int_open_file();
									break;
								
								case 0x07:
									int_wait_pid();
									break;
								
								case 0x0B:
									if (!int_execve())
										return;
									break;
										
								case 0x2D:
									int_sys_brk();
									break;
									
								default:
								    print_trace(stdout);
									printf("Unknown system call %02x\n", _eax);
									return;
							}
							break;
						
						default:
						    print_trace(stdout);
							printf("Unknown interupt %02x\n", opcode);
							return;
					}
					break;
				
				case 0xE8:
					{
						int32_t offset = (int32_t)getLongPC();
						_process->push(_pc);
						_pc += offset;
						trace(" call %08x\n", _pc);
					}
					break;
					
				case 0xE9:
					{
						int32_t offset = (int32_t)getLongPC();
						_pc += offset;
						trace(" jmp %08x\n", _pc);
					}
					break;
					
				case 0xEB:
					opcode = getPC();
					trace(" jmp\n");
					_pc = _pc + opcode - (opcode >= 0x80 ? 0x100 : 0);
					trace("  => jump to %08x\n", _pc);
					break;

				case 0xF7:
					opcode = getPC();
					switch (opcode)
					{
						case 0xD5:
							_ebp = ~_ebp;
							trace(" bitwnot ebp\n", opcode, _ebp);
							break;
							
						default:
							unknownOpcode();
							return;
					}
					break;
					
				default:
					unknownOpcode();
					return;
			}
		}
	}
	
	void int_open_file()
	{
		char filename[500];
		uint32_t is = _ebx;
		for (int i = 0; i < 499; i++, is++)
		{
			char ch = _process->loadByte(is);
			filename[i] = ch;
			if (ch == '\0')
				break;
		}
		File *file = getFile(filename);
		bool read_only = (_ecx & O_ACCMODE) == O_RDONLY;
		Usage *usage = new Usage(file, _process);
		if (read_only)
			usage->is_input(0);
		else
			usage->is_output(0);
			
		if (file->path == 0)
			file->path = copystr(read_only ? name_in_root(filename) : file->name);
		printf(" Open ProgramFile %s %d %d =>", file->path, _ecx, _edx);
		int fh = open(file->path, _ecx, _edx);
		printf(" %d\n", fh);
		if (fh <= 0)
		{
			exit(0);
		}
		file->fh = fh;
		_eax = fh;
	}
	
	void int_wait_pid()
	{
		_eax = 0;
	}
	
	void int_fork()
	{
		_process->pc = _pc;
		_process = newProcess(_process);
		_eax = 0;
	}
	
	void int_read()
	{
		uint32_t i = 0;
		for (; i < _edx; i++)
		{
			byte buffer;
			int r = read(_ebx, &buffer, 1);
			if (r == -1)
			{
				_eax = -4; // EOF
				printf(" read errno\n");
				return;
			}
			if (r == 0)
				break;
			_process->storeByte(_ecx + i, buffer);
			if (buffer > ' ' && buffer < 127)
				trace(" read %02x: %c to %08x\n", buffer, buffer, _ecx + i);
			else
				trace(" read %02x to %08x\n", buffer, _ecx + i);
		}
		if (i == 0)
			_eax = -4; // EOF
		trace(" read %d bytes\n", i);
		_eax = i;
	}		

	void int_write()
	{
		uint32_t i = 0;
		for (; i < _edx; i++)
		{
			byte buffer = _process->loadByte(_ecx + i);
			if (buffer > ' ' && buffer < 127)
				trace(" write %02x: %c from %08x\n", buffer, buffer, _ecx + i);
			else
				trace(" write %02x from %08x\n", buffer, _ecx + i);
			int r = write(_ebx, &buffer, 1);
			if (r == -1)
			{
				_eax = errno;
				printf(" write errno %d\n", _eax);
				return;
			}
		}
		trace(" wrote %d bytes\n", i);
		_eax = i;
	}
	
	bool int_execve()
	{
		char prog_name[500];
		for (int i = 0; i < 200; i++)
		{
			prog_name[i] = _process->loadByte(_ebx + i);
			if (prog_name[i] == 0)
				break;
		}

		printf(" execve\n  |%s|\n", prog_name);
		
#define MAX_ARG_LEN 500
		
		int argc = 0;
		while (_process->loadDWord(_ecx + 4 * argc) != 0)
			argc++;
		char **argv = (char **)malloc(sizeof(char *) * (argc + 1));
		argv[argc] = 0;
		
		for (int j = 0; j < argc; j++)
		{
			uint32_t addr = _process->loadDWord(_ecx + 4 * j);

			char arg[MAX_ARG_LEN];
			int i = 0;
			for (; i < MAX_ARG_LEN; i++)
			{
				arg[i] = _process->loadByte(addr + i);
				if (arg[i] == 0)
					break;
			}
			printf(" %d |%s|\n", j, arg);
			argv[j] = copystr(arg);
		}
		
#define MAX_ENV_LEN 500
		
		int envc = 0;
		while (_process->loadDWord(_edx + 4 * envc) != 0)
			envc++;
		char **env = (char **)malloc(sizeof(char *) * (envc + 1));
		env[envc] = 0;
		
		for (int j = 0; j < envc; j++)
		{
			uint32_t addr = _process->loadDWord(_edx + 4 * j);

			char arg[MAX_ENV_LEN];
			int i = 0;
			for (; i < MAX_ENV_LEN; i++)
			{
				arg[i] = _process->loadByte(addr + i);
				if (arg[i] == 0)
					break;
			}
			printf(" env %d |%s|\n", j, arg);
			env[j] = copystr(arg);
		}
		
		File *file = getFile(prog_name);
		Usage *usage = new Usage(file, _process);
		usage->is_exec(0);
		if (file->path == 0)
			file->path = copystr(name_in_root(prog_name));
		
		ProgramFile program;
		if (!program.open(file->path))
		{
			fprintf(stderr, "Could not read '%s'\n", file->path);
			return false;
		}
		
		if (!loadELF(&program, _process))
		{
			fprintf(stderr, "Failed to load '%s' as ELF\n", file->path);
			return false;
		}
		
		_process->init(argc, argv, env);
		_pc = _process->pc;
		
		return true;
	}
	
	void int_sys_brk()
	{
		trace(" sys_brk %08x:", _ebx);
		if (_ebx < _process->end_code)
		{
			_eax = _process->brk;
			trace(" init on");
		}
		else
		{
			_eax = (_ebx + 0xfff) & 0xfffff000;
			if (_eax > _process->brk)
			{
				trace(" extend to");
				_process->brk = _eax;
			}
		}
		trace(" %08x\n", _eax);
	}
	
	void unknownOpcode()
	{
		print_trace(stdout);
		printf("Unknown opcode in %s\n", _process->name);
	}

private:
	byte getPC()
	{
		byte v = _process->loadByte(_pc);
		trace("pc = %08x %02x\n", _pc, v);
		_pc++;
		return v;
	}
	unsigned short getShortPC()
	{
		unsigned short val = getPC();
		val |= (unsigned short)getPC() << 8;
		return val;
	}
	uint32_t getLongPC()
	{
		uint32_t val = getShortPC();
		val |= (uint32_t)getShortPC() << 16;
		return val;
	}
	
	void set_al(byte val) { _eax = (_eax & 0xffffff00L) | (uint32_t)val; trace(" eax = %08x\n", _eax); } 
	void set_cx(unsigned short val) { _ecx = (_ecx & 0xffff0000L) | (uint32_t)val; trace(" ecx = %08x\n", _ecx); } 
	void set_dx(unsigned short val) { _edx = (_edx & 0xffff0000L) | (uint32_t)val; trace(" edx = %08x\n", _edx);} 

	Process *_process;
	uint32_t _pc;
	uint32_t _eax;
	uint32_t _ebx;
	uint32_t _ecx;
	uint32_t _edx;
	uint32_t _esi;
	uint32_t _edi;
	uint32_t _ebp;
	int32_t _flags;
};

int main(int argc, char *argv[])
{
	if (argc < 3)
	{
		fprintf(stderr, "No argument\n");
		return 0;
	}

	root_dir = argv[1];
		
	Process *main_process = newProcess(0);
	
	File *file = getFile(argv[2]);
	Usage *usage = new Usage(file, main_process);
	usage->is_exec(0);
	if (file->path == 0)
		file->path = copystr(name_in_root(argv[2]));

	ProgramFile program;
	if (!program.open(file->path))
	{
		fprintf(stderr, "Could not read '%s'\n", argv[2]);
		return 0;
	}
	
	if (!loadELF(&program, main_process))
	{
		fprintf(stderr, "Failed to load '%s' as ELF\n", argv[2]);
		return 0;
	}
	
	char *env[1] = { 0 };
	main_process->init(argc - 2, argv + 2, env);
	
	Processor processor(main_process);
	processor.run();
	
	return 0;	
}
