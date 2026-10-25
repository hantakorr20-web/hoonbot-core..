// gpci.cpp - exact copy of the working RakSAMP gen_gpci + BIG_NUM_MUL (client/src/math_stuff.cpp, misc_funcs.cpp).
// The old workflow replaced BIG_NUM_MUL with a different formula, which produced 47-48 char GPCI strings
// instead of the 42-43 chars a real client sends, so the server kicked the bot right after join.
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>

unsigned int _uiRndSrvChallenge = 0;

void BIG_NUM_MUL(uint32_t in[5], uint32_t out[6], uint32_t factor)
{
	/*
		Based on TTMath library by Tomasz Sowa.
	*/

	uint32_t src[5] = {0};
	for (int i = 0; i < 5; i++)
		src[i] = ((in[4-i]>>24) | ((in[4-i]<<8) & 0x00FF0000) | ((in[4-i]>>8) & 0x0000FF00) | (in[4-i]<<24));

	uint64_t tmp = 0;

	tmp = static_cast<uint64_t>(src[0])*static_cast<uint64_t>(factor);
	out[0] = tmp&0xFFFFFFFF;
	out[1] = tmp>>32;
	tmp = static_cast<uint64_t>(src[1])*static_cast<uint64_t>(factor) + static_cast<uint64_t>(out[1]);
	out[1] = tmp&0xFFFFFFFF;
	out[2] = tmp>>32;
	tmp = static_cast<uint64_t>(src[2])*static_cast<uint64_t>(factor) + static_cast<uint64_t>(out[2]);
	out[2] = tmp&0xFFFFFFFF;
	out[3] = tmp>>32;
	tmp = static_cast<uint64_t>(src[3])*static_cast<uint64_t>(factor) + static_cast<uint64_t>(out[3]);
	out[3] = tmp&0xFFFFFFFF;
	out[4] = tmp>>32;
	tmp = static_cast<uint64_t>(src[4])*static_cast<uint64_t>(factor) + static_cast<uint64_t>(out[4]);
	out[4] = tmp&0xFFFFFFFF;
	out[5] = tmp>>32;

	for (int i = 0; i < 12; i++)
	{
		unsigned char temp = ((unsigned char*)out)[i];
		((unsigned char*)out)[i] = ((unsigned char*)out)[23 - i];
		((unsigned char*)out)[23 - i] = temp;
	}
}


static void gpci_seed_once()
{
	static bool seeded = false;
	if (!seeded) { seeded = true; srand((unsigned)time(0) ^ ((unsigned)getpid() << 8)); }
}

int gen_gpci(char buf[64], unsigned long factor) /* by bartekdvd */
{
	gpci_seed_once();
	unsigned char out[6*4] = {0};

	static const char alphanum[] =
		"0123456789"
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ";

	for (int i = 0; i < 6*4; ++i)
		out[i] = alphanum[rand() % (sizeof(alphanum) - 1)];

	

	BIG_NUM_MUL((uint32_t*)out, (uint32_t*)out, (uint32_t)factor);

	unsigned int notzero = 0;
	buf[0] = '0'; buf[1] = '\0';

	if (factor == 0) return 1;

	int pos = 0;
	for (int i = 0; i < 24; i++)
	{
		unsigned char tmp = out[i] >> 4;
		unsigned char tmp2 = out[i]&0x0F;
		
		if (notzero || tmp)
		{
			buf[pos++] = (char)((tmp > 9)?(tmp + 55):(tmp + 48));
			if (!notzero) notzero = 1;
		}

		if (notzero || tmp2)
		{
			buf[pos++] = (char)((tmp2 > 9)?(tmp2 + 55):(tmp2 + 48));
			if (!notzero) notzero = 1;
		}
	}
	buf[pos] = 0;

	return pos;
}
