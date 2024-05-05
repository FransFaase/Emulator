#include <stdio.h>
#include <string.h>
#include <ctype.h>

char inst[2000][9];
int nr_inst = 0;

int main(int argc, char *argv[])
{
	char buffer[2000];
	FILE *f = fopen("Emulator.cpp", "r");
	bool proc = false;
	char line[9];
	while (fgets(buffer, 999, f))
	{
		int ind = 0;
		char *s = buffer;
		for ( ;*s == '\t'; s++)
			ind++;
		if (ind == 3 && strncmp(s, "switch (opcode)", 15) == 0)
			proc = true;
		else if (ind == 3 && *s == '}')
			proc = false; 
		else if (proc && strncmp(s, "case 0x", 7) == 0)
		{
			if (ind == 4)
				sprintf(line, "%c%c", tolower(s[7]), tolower(s[8]));
			else if (ind == 6)
				sprintf(line + 2, " %c%c", tolower(s[7]), tolower(s[8]));
			else
				sprintf(line + 5, " %c%c", tolower(s[7]), tolower(s[8]));
			if (nr_inst > 0 && strncmp(inst[nr_inst-1], line, strlen(inst[nr_inst-1])) == 0)
				nr_inst--;
			strcpy(inst[nr_inst++], line);
		}
	}
	fclose(f);

	while (fgets(buffer, 999, stdin))
	{
		if (buffer[8] == ':' && buffer[9] == '\t')
		{
			bool found = false;
			for (int i = 0; i < nr_inst; i++)
				if (strncmp(buffer + 10, inst[i], strlen(inst[i])) == 0)
				{
					found = true;
					break;
				}
			if (!found)
				printf("%s", buffer + 10);
		}
	}
	return 0;
}