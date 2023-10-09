#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstdint>

typedef unsigned char byte;

class Process
{
public:
	byte **memory[256];
	
	uint32_t start_pc;
	uint32_t sp;
	
	Process()
	{
		for (int i = 0; i < 256; i++)
			memory[i] = 0;
		sp = 0xFFFFFFFFL;
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
	
	byte loadByte(long address)
	{
		byte hi = (byte)((address >> 24) & 0xff);
		if (memory[hi] == 0)
			return 0;
		byte hi2 = (byte)((address >> 16) & 0xff);
		if (memory[hi][hi2] == 0)
			return 0;
		return memory[hi][hi2][(address & 0xffff)];
	}

	void storeByte(long address, byte value)
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

	void push(uint32_t value)
	{
		sp -= 4;
		printf("push %08x to %08x\n", value, sp);
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
		printf("pop %08x from %08x\n", value, sp);
		sp += 4;
		
		return value;
	}
};


class File
{
public:
	byte *data = 0;
	uint32_t length;
	
	File() : data(0), length(0) {}
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
			printf("%02X ", data[i]);
		printf("\n");
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
		printf("readLong %08x ", i);
		uint32_t result = 0;
		result |= (uint32_t)data[i++];
		result |= (uint32_t)data[i++] << 8;
		result |= (uint32_t)data[i++] << 16;
		result |= (uint32_t)data[i++] << 24;
		printf("%08x\n", result);
		return result;
	}
};

bool loadELF(File *file, Process *process)
{
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
	process->start_pc = pc;
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
			printf("Load byte from %08x: %02x ", from_file, file->data[from_file]);
			process->storeByte(to_mem, file->data[from_file]);
			printf("Stored at %08x: %02x\n", to_mem, process->loadByte(to_mem)); 
			from_file++;
			to_mem++;
		}
	}
	
	return true;
}

class Processor
{
public:
	Processor(Process *process) : _process(process) {}
	
	void run()
	{
		_pc = _process->start_pc;
		for (;;)
		{
			byte opcode = getPC();
			
			switch (opcode)
			{
				case 0x01:
					opcode = getPC();
					switch (opcode)
					{
						case 0xF8:
							_eax += _edi;
							printf(" add_eax,edi %08x\n", _eax);
							break;
							
						default:
							printf("Unknown opcode\n");
							return;
					}
					break;
				
				case 0x2C:
					opcode = getPC();
					printf(" sub_al, %d\n", opcode);
					set_al((byte)(_eax & 0xFF) - opcode);
					break;
					 
				case 0x31:
					opcode = getPC();
					switch(opcode)
					{
						case 0xC9:
							_ecx = 0L;
							printf(" xor_ecx,ecx\n");
							break;
						
						case 0xD2:
							_edx = 0L;
							printf(" xor_edx,edx\n");
							break;

						case 0xDB:
							_ebx = 0L;
							printf(" xor_ebx,ebx\n");
							break;

						case 0xED:
							_ebp = 0L;
							printf(" xor_ebp,ebp\n");
							break;

						case 0xFF:
							_edi = 0L;
							printf(" xor_edi,edi\n");
							break;

						default:
							printf("Unknown opcode\n");
							return;
					}
					break;
					
				case 0x3C:
					opcode = getPC();
					_flags = (byte)_eax - opcode;
					printf(" cmp_al, %d = %08x\n", opcode, _flags);
					break;
					
				case 0x4D:
					_ebp--;
					break;
					
				case 0x50:
					_process->push(_eax);
					printf(" push_eax\n");
					break;
					
				case 0x52:
					_process->push(_edx);
					printf(" push_edx\n");
					break;
					
				case 0x53:
					_process->push(_ebx);
					printf(" push_ebx\n");
					break;
					
				case 0x58:
					_eax = _process->pop();
					printf(" pop_eax: %08x\n", _eax);
					break;
					
				case 0x5A:
					_edx = _process->pop();
					printf(" pop_edx: %08x\n", _ebx);
					break;
					
				case 0x5B:
					_ebx = _process->pop();
					printf(" pop_ebx: %08x\n", _ebx);
					break;
					
				case 0x5D:
					_ebp = _process->pop();
					printf(" pop_eax: %08x\n", _eax);
					break;
					
				case 0x6A:
					opcode = getPC();
					_process->push((uint32_t)(char)opcode);
					printf(" push %02x\n", opcode);
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
							printf("Unknown opcode\n");
							return;
					}
					break;
				
				case 0x74:
					opcode = getPC();
					printf(" je %02X\n", opcode);
					if (_flags == 0)
					{
						_pc = _pc + opcode - (opcode >= 0x80 ? 0x100 : 0);
						printf("  => jump to %08x\n", _pc);
					}
					break;
				
				case 0x75:
					opcode = getPC();
					printf(" jne %02X\n", opcode);
					if (_flags != 0)
					{
						_pc = _pc + opcode - (opcode >= 0x80 ? 0x100 : 0);
						printf("  => jump to %08x\n", _pc);
					}
					break;
				
				case 0x7C:
					opcode = getPC();
					printf(" jl %02X  flags = %d\n", opcode, _flags);
					if (_flags < 0)
					{
						_pc = _pc + opcode - (opcode >= 0x80 ? 0x100 : 0);
						printf("  => jump to %08x\n", _pc);
					}
					break;
				
				case 0x7D:
					opcode = getPC();
					printf(" jge %02X\n", opcode);
					if (_flags >= 0)
					{
						_pc = _pc + opcode - (opcode >= 0x80 ? 0x100 : 0);
						printf("  => jump to %08x\n", _pc);
					}
					break;
				
				case 0x85:
					opcode = getPC();
					switch (opcode)
					{
						case 0xC0:
							_flags = (int32_t)_eax;
							printf(" test_eax,eax %08x %d\n", _eax, _flags);
							break;
							
						case 0xED:
							_flags = (int32_t)_ebp;
							printf(" test_ebp,ebp\n");
							break;
							
						default:
							printf("Unknown opcode\n");
							return;
					}
					break;
						
				case 0x89:
					opcode = getPC();
					switch (opcode)
					{
						case 0xC2: _edx = _eax; printf(" mov_edx,eax = %08x\n", _edx); break;
						case 0xC6: _esi = _eax; printf(" mov_esi,eax = %08x\n", _esi);break;
						case 0xC7: _edi = _eax; printf(" mov_edi,eax = %08x\n", _edi);break;
						case 0xD3: _ebx = _edx; printf(" mov_edx,eax = %08x\n", _edx);break;
						case 0xE1: _ecx = _process->sp; printf(" mov_ecx,esp = %08x\n", _ecx);break;
						case 0xF3: _ebx = _esi; printf(" mov_ebx,esi = %08x\n", _ebx);break;
							
						default:
							printf("Unknown opcode\n");
							return;
					}
					break;
				
				case 0xC1:
					opcode = getPC();
					switch (opcode)
					{
						case 0xE7: 
							opcode = getPC();
							_edi = _edi << opcode;
							printf(" shl_edi, %d %08x\n", opcode, _edi);
							break;
							
						default:
							printf("Unknown opcode\n");
							return;
					}
					break;
					
				case 0xC3:
					_pc = _process->pop();
					printf(" ret to %02x\n", _pc);
					break;
					
				case 0xCD:
					opcode = getPC();
					printf(" int %02x\n", opcode);
					switch (opcode)
					{
						case 0x80:
							switch (_eax)
							{
								case 0x01:
									// Exit
									return;
									
								case 0x03:
									int_read();
									break;
								
								case 0x04:
									int_write();
									break;
								
								case 0x05:
									int_open_file();
									break;
									
								default:
									printf("Unknown system call %02x\n", _eax);
									return;
							}
							break;
						
						default:
							printf("Unknown interupt %02x\n", opcode);
							return;
					}
					break;
				
				case 0xE8:
					{
						int32_t offset = (int32_t)getLongPC();
						_process->push(_pc);
						_pc += offset;
						printf(" call %08x\n", _pc);
					}
					break;
					
				case 0xEB:
					opcode = getPC();
					printf(" jmp\n");
					_pc = _pc + opcode - (opcode >= 0x80 ? 0x100 : 0);
					printf("  => jump to %08x\n", _pc);
					break;

				default:
					printf("Unknown opcode\n");
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
		printf(" Open File %s %d %d =>", filename, _ecx, _edx);
		int fh = open(filename, _ecx, _edx);
		printf(" %d\n", fh);
		if (fh <= 0)
		{
			exit(0);
		}
		_eax = fh;
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
				_eax = errno;
				printf(" read errno %d\n", _eax);
				return;
			}
			if (r == 0)
				break;
			_process->storeByte(_ecx + i, buffer);
			printf(" read %02x", buffer);
			if (buffer > ' ' && buffer < 127)
				printf(": %c", buffer);
			printf(" to %08x\n", _ecx + i);
		}
		printf(" read %d bytes\n", i);
		_eax = i;
	}		

	void int_write()
	{
		uint32_t i = 0;
		for (; i < _edx; i++)
		{
			byte buffer = _process->loadByte(_ecx + i);
			printf(" write %02x", buffer);
			if (buffer > ' ' && buffer < 127)
				printf(": %c", buffer);
			printf(" from %08x\n", _ecx + i);
			int r = write(_ebx, &buffer, 1);
			if (r == -1)
			{
				_eax = errno;
				printf(" write errno %d\n", _eax);
				return;
			}
		}
		printf(" wrote %d bytes\n", i);
		_eax = i;
	}		

private:
	byte getPC()
	{
		byte v = _process->loadByte(_pc);
		printf("pc = %08x %02x\n", _pc, v);
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
	
	void set_al(byte val) { _eax = (_eax & 0xffffff00L) | (uint32_t)val; printf(" eax = %08x\n", _eax); } 
	void set_cx(unsigned short val) { _ecx = (_ecx & 0xffff0000L) | (uint32_t)val; printf(" ecx = %08x\n", _ecx); } 
	void set_dx(unsigned short val) { _edx = (_edx & 0xffff0000L) | (uint32_t)val; printf(" edx = %08x\n", _edx);} 

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
	if (argc == 1)
	{
		fprintf(stderr, "No argument\n");
		return 0;
	}
	
	File program;
	if (!program.open(argv[1]))
	{
		fprintf(stderr, "Could not read '%s'\n", argv[1]);
		return 0;
	}
	
	Process main_process;
	
	if (!loadELF(&program, &main_process))
	{
		fprintf(stderr, "Failed to load '%s' as ELF\n", argv[1]);
		return 0;
	}
	
	// File process with arguments
	long p = 0;
	for (int i = argc - 1; i > 0; i--)
	{
		main_process.push(p);
		for (int j = 0; ; j++)
		{
			main_process.storeByte(p++, argv[i][j]);
			if (argv[i][j] == 0)
				break;
		}
	}
	main_process.push(argc - 1);
	
	Processor processor(&main_process);
	processor.run();
	
	return 0;	
}
