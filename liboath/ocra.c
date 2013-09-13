/*
 * ocra.c - implementation of the OATH OCRA algorithm
 * Copyright (C) 2013 Fabian Grünbichler
 * Copyright (C) 2013 Simon Josefsson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include <config.h>

#include "oath.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>

#include "gc.h"

static int
map_hash (int h, int *len)
{
  switch (h)
    {
    case 1:
      if (len)
	*len = GC_SHA1_DIGEST_SIZE;
      return OATH_OCRA_HASH_SHA1;
    case 256:
      if (len)
	*len = GC_SHA256_DIGEST_SIZE;
      return OATH_OCRA_HASH_SHA256;
    case 512:
      if (len)
	*len = GC_SHA512_DIGEST_SIZE;
      return OATH_OCRA_HASH_SHA512;
    default:
      return -1;
    }
}

static int
map_challtype (char c)
{
  switch (c)
    {
    case 'A':
      return OATH_OCRA_CHALLENGE_ALPHANUM;
    case 'N':
      return OATH_OCRA_CHALLENGE_NUM;
    case 'H':
      return OATH_OCRA_CHALLENGE_HEX;
    default:
      return -1;
    }
}

struct oath_ocrasuite_st
{
  /* A copy of the OCRASuite string.  */
  char *ocrasuite_str;
  /* Defines which hash function is used to calculate the HMAC value
     on which the OCRA value is based. */
  oath_ocra_hash_t ocra_hash;
  /* Length of OCRA value (0 == no truncation, full HMAC length). */
  int digits;
  /* Flag indicating whether a counter value is used as data input. */
  bool use_counter;
  /* Defines which hash function is used for password hashes.
     OATH_OCRA_HAS_NONE means no password hash is included as data
     input. */
  oath_ocra_hash_t password_hash;
  /* Defines challenge type, see %oath_ocra_challenge_t. */
  oath_ocra_challenge_t challenge_type;
  /* Defines length of one challenge string. */
  size_t challenge_length;
  /* Divisor used to calculate timesteps passed since beginning of
     epoch (0 means no timestamp included as data input). */
  uint16_t time_step_size;
  /* Number of bytes of session information used as data input. */
  size_t session_length;
  /* Total length of data input (in bytes). */
  size_t datainput_length;
};

static int
parse_ocrasuite (const char *ocrasuite, oath_ocrasuite_t * ocrasuite_info)
{
  const char *tmp;
  char f, Gunit;
  unsigned h, n, xx, nnn, G, consumed;

  memset (ocrasuite_info, 0, sizeof (*ocrasuite_info));

  ocrasuite_info->ocrasuite_str = (char *) ocrasuite;

  ocrasuite_info->datainput_length = strlen (ocrasuite) + 1 + 128;

  if (sscanf (ocrasuite, "OCRA-1:HOTP-SHA%u-%u:%n", &h, &n, &consumed) != 2)
    return OATH_SUITE_PARSE_ERROR;

  ocrasuite_info->ocra_hash = map_hash (h, NULL);
  if ((int) ocrasuite_info->ocra_hash == -1)
    return OATH_SUITE_PARSE_ERROR;

  if (n != 0 && (n < 4 || n > 10))
    return OATH_SUITE_PARSE_ERROR;
  ocrasuite_info->digits = n;

  tmp = ocrasuite + consumed;
  if (strncmp (tmp, "C-", 2) == 0)
    {
      ocrasuite_info->datainput_length += 8;
      ocrasuite_info->use_counter = true;
      tmp += 2;
    }

  if (sscanf (tmp, "Q%c%02u-PSHA%u-S%03u-T%u%[HMS]%n",
	      &f, &xx, &h, &nnn, &G, &Gunit, &consumed) == 6)
    {
    }
  else if (sscanf (tmp, "Q%c%02u-PSHA%u%n", &f, &xx, &h, &consumed) == 3)
    {
      G = 0;
      nnn = 0;
    }
  else if (sscanf (tmp, "Q%c%02u-T%02u%[HMS]%n",
		   &f, &xx, &G, &Gunit, &consumed) == 4)
    {
      h = 0;
      nnn = 0;
    }
  else if (sscanf (tmp, "Q%c%02u%n", &f, &xx, &consumed) == 2)
    {
      G = 0;
      h = 0;
      nnn = 0;
    }
  else
    return OATH_SUITE_PARSE_ERROR;

  if (tmp[consumed] != '\0')
    return OATH_SUITE_PARSE_ERROR;

  ocrasuite_info->challenge_type = map_challtype (f);
  if ((int) ocrasuite_info->challenge_type == -1)
    return OATH_SUITE_PARSE_ERROR;

  if (xx < 4 || xx > 64)
    return OATH_SUITE_PARSE_ERROR;
  ocrasuite_info->challenge_length = xx;

  if (nnn > 512)
    return OATH_SUITE_PARSE_ERROR;
  ocrasuite_info->session_length = nnn;
  ocrasuite_info->datainput_length += nnn;

  if (h)
    {
      int len;

      ocrasuite_info->password_hash = map_hash (h, &len);
      if ((int) ocrasuite_info->password_hash == -1)
	return OATH_SUITE_PARSE_ERROR;
      ocrasuite_info->datainput_length += len;
    }

  if (G)
    {
      ocrasuite_info->time_step_size = G;
      switch (Gunit)
	{
	case 'S':
	  if (G == 0 || G > 59)
	    return OATH_SUITE_PARSE_ERROR;
	  break;

	case 'M':
	  if (G == 0 || G > 59)
	    return OATH_SUITE_PARSE_ERROR;
	  ocrasuite_info->time_step_size *= 60;
	  break;

	case 'H':
	  /* RFC 6287 says H=00 is permitted but that is nonsensical. */
	  if (G == 0 || G > 48)
	    return OATH_SUITE_PARSE_ERROR;
	  ocrasuite_info->time_step_size *= 60 * 60;
	  break;

	default:
	  return OATH_SUITE_PARSE_ERROR;
	}

      ocrasuite_info->datainput_length += 8;
    }

  return OATH_OK;
}

/**
 * oath_ocrasuite_parse:
 * @ocrasuite_string: OCRASuite string to be parsed.
 * @osh: Output pointer to OCRASuite handle.
 *
 * Parses the zero-terminated string @ocrasuite_string, storing the
 * results in the @osh handle.  OCRA Suite strings are explained in
 * RFC 6287.  Two example strings would be
 * "OCRA-1:HOTP-SHA1-4:QH8-S512" and
 * "OCRA-1:HOTP-SHA512-8:C-QN08-PSHA1".
 *
 * Returns: On success, %OATH_OK (zero) is returned, otherwise an
 * error code is returned.
 *
 * Since: 2.6.0
 **/
int
oath_ocrasuite_parse (const char *ocrasuite, oath_ocrasuite_t ** osh)
{
  int rc;

  if (ocrasuite == NULL || osh == NULL)
    return OATH_SUITE_PARSE_ERROR;

  *osh = calloc (1, sizeof (**osh));
  if (*osh == NULL)
    return OATH_MALLOC_ERROR;

  rc = parse_ocrasuite (ocrasuite, *osh);
  if (rc != OATH_OK)
    {
      free (*osh);
      return rc;
    }

  (*osh)->ocrasuite_str = strdup ((*osh)->ocrasuite_str);
  if ((*osh)->ocrasuite_str == NULL)
    {
      free (*osh);
      return OATH_MALLOC_ERROR;
    }

  return OATH_OK;
}

/**
 * oath_ocrasuite_done:
 * @osh: OCRASuite handle.
 *
 * Releases all resources associated with the given @osh OCRASuite
 * handle.
 *
 * Since: 2.6.0
 **/
void
oath_ocrasuite_done (oath_ocrasuite_t * osh)
{
  free (osh->ocrasuite_str);
  free (osh);
}

/**
 * oath_ocrasuite_get_cryptofunction_hash:
 * @osh: OCRASuite handle.
 *
 * Get the hash function used for an HOTP CryptoFunction.
 *
 * Returns: An %oath_ocra_hash_t hash function.
 *
 * Since: 2.6.0
 **/
oath_ocra_hash_t
oath_ocrasuite_get_cryptofunction_hash (oath_ocrasuite_t * osh)
{
  return osh->ocra_hash;
}

/**
 * oath_ocrasuite_get_cryptofunction_digits:
 * @osh: OCRASuite handle.
 *
 * Get the truncation size for an HOTP CryptoFunction.  This is the
 * output size of the OCRA code, e.g., 6 means the output code is 6
 * digits.
 *
 * Returns: Size of output code.
 *
 * Since: 2.6.0
 **/
int
oath_ocrasuite_get_cryptofunction_digits (oath_ocrasuite_t * osh)
{
  return osh->digits;
}

/**
 * oath_ocrasuite_get_counter:
 * @osh: OCRASuite handle.
 *
 * Get whether a counter is used for the OCRASuite.
 *
 * Returns: true if a counter is used, false otherwise.
 *
 * Since: 2.6.0
 **/
bool
oath_ocrasuite_get_counter (oath_ocrasuite_t * osh)
{
  return osh->use_counter;
}

/**
 * oath_ocrasuite_get_challenge_type:
 * @osh: OCRASuite handle.
 *
 * Get the challenge format in the @osh OCRASuite.
 *
 * Returns: a %oath_ocra_challenge_t value, e.g.,
 * #OATH_OCRA_CHALLENGE_ALPHANUM.
 *
 * Since: 2.6.0
 **/
oath_ocra_challenge_t
oath_ocrasuite_get_challenge_type (oath_ocrasuite_t * osh)
{
  return osh->challenge_type;
}

/**
 * oath_ocrasuite_get_challenge_length:
 * @osh: OCRASuite handle.
 *
 * Get the maximum length of the challenge of the OCRASuite, 04-64.
 *
 * Returns: challenge length.
 *
 * Since: 2.6.0
 **/
size_t
oath_ocrasuite_get_challenge_length (oath_ocrasuite_t * osh)
{
  return osh->challenge_length;
}

/**
 * oath_ocrasuite_get_password_hash:
 * @osh: OCRASuite handle.
 *
 * Get the hash function used for the PIN/password.
 *
 * Returns: a %oath_ocra_hash_t value, e.g., #OATH_OCRA_HASH_SHA1.
 *
 * Since: 2.6.0
 **/
oath_ocra_hash_t
oath_ocrasuite_get_password_hash (oath_ocrasuite_t * osh)
{
  return osh->password_hash;
}

/**
 * oath_ocrasuite_get_session_length:
 * @osh: OCRASuite handle.
 *
 * Get the length of the session data in the OCRASuite.
 *
 * Returns: length of the session, typical values are 64, 128, 256 and
 * 512.
 *
 * Since: 2.6.0
 **/
int
oath_ocrasuite_get_session_length (oath_ocrasuite_t * osh)
{
  return osh->session_length;
}

/**
 * oath_ocra_challenge_generate:
 * @challtype: a %oath_ocra_challenge_t type, e.g., #OATH_OCRA_CHALLENGE_HEX.
 * @length: length of challenge to generate.
 * @challenge: Output buffer, needs space for 65 chars.
 *
 * Generates a (pseudo)random challenge string of length @length and
 * type @challtype.
 *
 * According to the RFC, challenges questions SHOULD be 20-byte values
 * and MUST be at least t-byte values where t stands for the
 * digit-length of the OCRA truncation output (i.e., @digits in a
 * parsed %oath_ocrasuite_t).
 *
 * Returns: %OATH_OK (zero) on success, an error code otherwise.
 *
 * Since: 2.6.0
 **/
int
oath_ocra_challenge_generate (oath_ocra_challenge_t challtype,
			      size_t length, char *challenge)
{
  const char *lookup =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  int wraplen;
  char *rng;
  uint8_t *p;
  size_t i;
  int rc;

  switch (challtype)
    {
    case OATH_OCRA_CHALLENGE_ALPHANUM:
      wraplen = sizeof (lookup) - 1;
      break;

    case OATH_OCRA_CHALLENGE_NUM:
      wraplen = 10;
      break;

    case OATH_OCRA_CHALLENGE_HEX:
      wraplen = 16;
      break;

    default:
      return OATH_INVALID_DIGITS;
      break;
    }

  rng = malloc (length);
  if (rng == NULL)
    return OATH_MALLOC_ERROR;

  rc = gc_nonce (rng, length);
  if (rc != GC_OK)
    {
      free (rng);
      return OATH_CRYPTO_ERROR;
    }

  p = (uint8_t *) rng;
  for (i = 0; i < length; i++)
    *challenge++ = lookup[*p++ % wraplen];
  *challenge = '\0';

  free (rng);

  return OATH_OK;
}

/**
 * oath_ocra_challenge_generate_suitestr:
 * @ocrasuite: String with OCRA Suite description.
 * @challenge: Output buffer, needs space for the challenge length
 *   in @ocrasuite plus one, which is max 65 bytes.
 *
 * Generates a (pseudo)random challenge string depending on the type
 * and length given by @ocrasuite.
 *
 * Returns: %OATH_OK (zero) on success, an error code otherwise.
 *
 * Since: 2.6.0
 **/
int
oath_ocra_challenge_generate_suitestr (const char *ocrasuite, char *challenge)
{
  int rc;
  oath_ocrasuite_t os;

  rc = parse_ocrasuite (ocrasuite, &os);
  if (rc != OATH_OK)
    return rc;

  return oath_ocra_challenge_generate (os.challenge_type,
				       os.challenge_length, challenge);
}

/**
 * oath_ocra_convert_challenge:
 * @challenge_type: Type of challenge, see %oath_ocra_challenge_t .
 * @challenge_string: Challenge string.
 * @challenge_binary_length: Length of returned byte-array.
 *
 * Converts @challenge_string to binary representation. Numerical values are
 * converted to base16 and then converted using %oath_hex2bin. Hexadecimal
 * values are simply converted using %oath_hex2bin, alpha numerical values are
 * just copied.
 *
 * Returns: malloc'ed byte-array of length @challenge_binary_length.
 *
 * Since: 2.6.0
 **/
char *
oath_ocra_convert_challenge (oath_ocra_challenge_t challenge_type,
			     const char *challenge_string,
			     size_t * challenge_binary_length)
{
  char *challenges = NULL;
  size_t challenge_length = strlen (challenge_string);
  switch (challenge_type)
    {
    case OATH_OCRA_CHALLENGE_NUM:
      {
	unsigned long int num_value = strtoul (challenge_string, NULL, 10);
	char *temp = malloc (challenge_length + 2);
	if (temp == NULL)
	  {
	    return NULL;
	  }
	sprintf (temp, "%lX", num_value);
	size_t hex_length = strlen (temp);
	if (hex_length % 2 == 1)
	  {
	    temp[hex_length] = '0';
	    temp[hex_length + 1] = '\0';
	  }
	oath_hex2bin (temp, NULL, challenge_binary_length);
	challenges = malloc (*challenge_binary_length);
	if (challenges == NULL)
	  {
	    free (temp);
	    return NULL;
	  }
	oath_hex2bin (temp, challenges, challenge_binary_length);
	free (temp);
      }
      break;
    case OATH_OCRA_CHALLENGE_HEX:
      {
	char *temp = malloc (challenge_length + 2);
	if (temp == NULL)
	  {
	    return NULL;
	  }
	memcpy (temp, challenge_string, challenge_length);
	temp[challenge_length] = '\0';
	if (challenge_length % 2 == 1)
	  {
	    temp[challenge_length] = '0';
	    temp[challenge_length + 1] = '\0';
	  }
	oath_hex2bin (temp, NULL, challenge_binary_length);
	challenges = malloc (*challenge_binary_length);
	if (challenges == NULL)
	  {
	    free (temp);
	    return NULL;
	  }

	oath_hex2bin (temp, challenges, challenge_binary_length);
	free (temp);
      }
      break;

    case OATH_OCRA_CHALLENGE_ALPHANUM:
      {
	*challenge_binary_length = challenge_length;
	challenges = malloc (*challenge_binary_length);
	if (challenges == NULL)
	  {
	    return NULL;
	  }

	memcpy (challenges, challenge_string, *challenge_binary_length);
      }
      break;
    default:
      break;
    }
  return challenges;
}

static int
oath_ocra_generate_internal (const char *secret,
			     size_t secret_length,
			     uint64_t counter,
			     const char *challenges,
			     size_t challenges_length,
			     const char *password_hash,
			     const char *session, time_t now,
			     oath_ocrasuite_t * parsed_suite,
			     char *output_ocra)
{
  int rc;
  char *byte_array = NULL;
  char *curr_ptr = NULL;
  uint64_t time_steps = 0;
  char tmp_str[17];
  size_t tmp_len;
  char *hs;
  size_t hs_size;
  uint8_t offset;
  long S;
  int otp_len;

  byte_array = malloc (parsed_suite->datainput_length);
  if (byte_array == NULL)
    {
      return OATH_MALLOC_ERROR;
    }

  curr_ptr = byte_array;
  memcpy (curr_ptr, parsed_suite->ocrasuite_str,
	  strlen (parsed_suite->ocrasuite_str));
  curr_ptr += strlen (parsed_suite->ocrasuite_str);
  curr_ptr[0] = '\0';
  curr_ptr++;
  if (parsed_suite->use_counter)
    {
      tmp_len = 8;
      sprintf (tmp_str, "%016" PRIX64, counter);
      oath_hex2bin (tmp_str, curr_ptr, &tmp_len);
      curr_ptr += 8;
    }

  if (challenges == NULL)
    {
      free (byte_array);
      return OATH_SUITE_MISMATCH_ERROR;
    }

  if (challenges_length > 128)
    {
      free (byte_array);
      return OATH_SUITE_MISMATCH_ERROR;
    }

  memcpy (curr_ptr, challenges, challenges_length);
  curr_ptr += challenges_length;
  if (challenges_length < 128)
    {
      memset (curr_ptr, '\0', (128 - challenges_length));
      curr_ptr += (128 - challenges_length);
    }

  if (parsed_suite->password_hash != OATH_OCRA_HASH_NONE
      && password_hash == NULL)
    {
      free (byte_array);
      return OATH_SUITE_MISMATCH_ERROR;
    }

  switch (parsed_suite->password_hash)
    {
    case OATH_OCRA_HASH_SHA1:
      memcpy (curr_ptr, password_hash, 20);
      curr_ptr += 20;
      break;
    case OATH_OCRA_HASH_SHA256:
      memcpy (curr_ptr, password_hash, 32);
      curr_ptr += 32;
      break;
    case OATH_OCRA_HASH_SHA512:
      memcpy (curr_ptr, password_hash, 64);
      curr_ptr += 64;
      break;
    default:
      break;
    }

  if (parsed_suite->session_length > 0)
    {
      if (session == NULL)
	{
	  free (byte_array);
	  return OATH_SUITE_MISMATCH_ERROR;
	}
      memcpy (curr_ptr, session, parsed_suite->session_length);
      curr_ptr += parsed_suite->session_length;
    }

  if (parsed_suite->time_step_size != 0)
    {
      time_steps = now / parsed_suite->time_step_size;
      tmp_len = 8;
      sprintf (tmp_str, "%016" PRIX64, time_steps);
      oath_hex2bin (tmp_str, curr_ptr, &tmp_len);
    }

  /*
    char hexstring[parsed_suite->datainput_length*2+1];
    oath_bin2hex(byte_array,parsed_suite->datainput_length,hexstring);

    printf("BYTE_ARRAY: %d\n",parsed_suite->datainput_length);
    printf(hexstring);
    printf("\n");
  */

  switch (parsed_suite->ocra_hash)
    {
    case OATH_OCRA_HASH_SHA1:
      hs_size = GC_SHA1_DIGEST_SIZE;
      hs = (char *) malloc (hs_size * sizeof (char));
      if (hs == NULL)
	{
	  free (byte_array);
	  return OATH_MALLOC_ERROR;
	}
      rc =
	gc_hmac_sha1 (secret, secret_length, byte_array,
		      parsed_suite->datainput_length, hs);
      break;
    case OATH_OCRA_HASH_SHA256:
      hs_size = GC_SHA256_DIGEST_SIZE;
      hs = (char *) malloc (hs_size * sizeof (char));
      if (hs == NULL)
	{
	  free (byte_array);
	  return OATH_MALLOC_ERROR;
	}
      rc =
	gc_hmac_sha256 (secret, secret_length, byte_array,
			parsed_suite->datainput_length, hs);
      break;
    case OATH_OCRA_HASH_SHA512:
      hs_size = GC_SHA512_DIGEST_SIZE;
      hs = (char *) malloc (hs_size * sizeof (char));
      if (hs == NULL)
	{
	  free (byte_array);
	  return OATH_MALLOC_ERROR;
	}
      rc =
	gc_hmac_sha512 (secret, secret_length, byte_array,
			parsed_suite->datainput_length, hs);
      break;
    default:
      free (byte_array);
      return OATH_SUITE_PARSE_ERROR;
    }

  free (byte_array);
  if (rc != 0)
    {
      return OATH_CRYPTO_ERROR;
    }

  offset = hs[hs_size - 1] & 0x0f;
  S = (((hs[offset] & 0x7f) << 24)
       | ((hs[offset + 1] & 0xff) << 16)
       | ((hs[offset + 2] & 0xff) << 8) | ((hs[offset + 3] & 0xff)));
  free (hs);
  switch (parsed_suite->digits)
    {
    case 4:
      S = S % 10000;
      break;
    case 5:
      S = S % 100000;
      break;
    case 6:
      S = S % 1000000;
      break;
    case 7:
      S = S % 10000000;
      break;
    case 8:
      S = S % 100000000;
      break;
    case 9:
      S = S % 1000000000;
      break;
    case 10:
      S = S % 10000000000;
      break;
    case 0:
      break;
    default:
      return OATH_INVALID_DIGITS;
      break;
    }

  otp_len = snprintf (output_ocra, parsed_suite->digits + 1, "%.*ld",
		      parsed_suite->digits, S);
  output_ocra[parsed_suite->digits] = '\0';
  if (otp_len <= 0 || otp_len != parsed_suite->digits)
    return OATH_PRINTF_ERROR;
  return OATH_OK;
}

/**
 * oath_ocra_generate:
 * @secret: The shared secret string.
 * @secret_length: Length of @secret.
 * @ocrasuite: String with information about used hash algorithms and input.
 * @counter: Counter value, optional (see @ocrasuite).
 * @challenges: Client/server challenge values, byte-array, mandatory.
 * @challenges_length: Length of @challenges.
 * @password_hash: Hashed password value, optional (see @ocrasuite).
 * @session: Static data about current session, optional (see @ocra-suite).
 * @now: Current timestamp, optional (see @ocrasuite).
 * @output_ocra: Output buffer.
 *
 * Generate a truncated hash-value used for challenge-response-based
 * authentication according to the OCRA algorithm described in RFC 6287.
 * Besides the mandatory challenge(s), additional input is optional.
 *
 * The string @ocrasuite denotes which mode of OCRA is to be used. Furthermore
 * it contains information about which of the possible optional data inputs are
 * to be used, and how.
 *
 * Numeric challenges must be converted to base16 before being passed as byte-array.
 *
 * The output buffer @output_ocra must have room for at least as many digits as
 * specified as part of @ocrasuite, plus one terminating NUL char.
 *
 * Returns: on success, %OATH_OK (zero) is returned, otherwise an error code is
 *   returned.
 *
 * Since: 2.6.0
 **/
int
oath_ocra_generate (const char *secret, size_t secret_length,
		    const char *ocrasuite, uint64_t counter,
		    const char *challenges,
		    size_t challenges_length, const char *password_hash,
		    const char *session, time_t now, char *output_ocra)
{
  int rc;
  oath_ocrasuite_t os;

  rc = parse_ocrasuite (ocrasuite, &os);
  if (rc != OATH_OK)
    return rc;

  return oath_ocra_generate_internal (secret, secret_length,
				      counter, challenges,
				      challenges_length, password_hash,
				      session, now, &os, output_ocra);
}

/**
 * oath_ocra_generate2:
 * @secret: The shared secret string.
 * @secret_length: Length of @secret.
 * @ocrasuite: String with information about used hash algorithms and input.
 * @counter: Counter value, optional (see @ocrasuite).
 * @challenges: Array of challenge strings, mandatory
 * @password_hash: Hashed password value, optional (see @ocrasuite).
 * @session: Static data about current session, optional (see @ocra-suite).
 * @now: Current timestamp, optional (see @ocrasuite).
 * @output_ocra: Output buffer.
 *
 * Generate a truncated hash-value used for challenge-response-based
 * authentication according to the OCRA algorithm described in RFC 6287.
 * Besides the mandatory challenge(s), additional input is optional.
 *
 * The string @ocrasuite denotes which mode of OCRA is to be used. Furthermore
 * it contains information about which of the possible optional data inputs are
 * to be used, and how.
 *
 * The challenge strings passed in @challenges are combined into one long
 * challenge string, which is then converted to binary. They must be
 * NUL-terminated.
 *
 * The output buffer @output_ocra must have room for at least as many digits as
 * specified as part of @ocrasuite, plus one terminating NUL char.
 *
 * Returns: on success, %OATH_OK (zero) is returned, otherwise an error code is
 *   returned.
 *
 * Since: 2.6.0
 **/

int
oath_ocra_generate2 (const char *secret, size_t secret_length,
		     const char *ocrasuite,
		     uint64_t counter, const char **challenges,
		     size_t challenges_count,
		     const char *password_hash,
		     const char *session, time_t now, char *output_ocra)
{

  int rc;
  oath_ocrasuite_t os;
  char chall_string[129];
  char *chall_bin;
  size_t chall_bin_length;

  size_t curr_length;
  int tmp = 0;

  if (challenges_count < 1)
    return -1;

  rc = parse_ocrasuite (ocrasuite, &os);
  if (rc != OATH_OK)
    return rc;

  while (challenges_count > 0 && tmp < 128)
    {
      curr_length = strlen (*challenges);
      memcpy (chall_string + tmp, *challenges, curr_length);
      tmp += curr_length;
      challenges++;
    }

  /* 2* max 64 chars limit */
  if (tmp >= 128)
    return -1;

  chall_string[tmp] = '\0';
  chall_bin = oath_ocra_convert_challenge (os.challenge_type,
					   chall_string, &chall_bin_length);

  if (chall_bin == NULL)
    return -1;

  return oath_ocra_generate_internal (secret, secret_length,
				      counter, chall_bin, chall_bin_length,
				      password_hash, session, now,
				      &os, output_ocra);
}

/**
 * oath_ocra_generate3:
 * @secret: The shared secret string.
 * @secret_length: Length of @secret.
 * @ocrasuite: String with information about used hash algorithms and input.
 * @counter: Counter value, optional (see @ocrasuite).
 * @challenges: Challenge string, mandatory
 * @password_hash: Hashed password value, optional (see @ocrasuite).
 * @session: Static data about current session, optional (see @ocra-suite).
 * @now: Current timestamp, optional (see @ocrasuite).
 * @output_ocra: Output buffer.
 *
 * Generate a truncated hash-value used for challenge-response-based
 * authentication according to the OCRA algorithm described in RFC 6287.
 * Besides the mandatory challenge(s), additional input is optional.
 *
 * The string @ocrasuite denotes which mode of OCRA is to be used. Furthermore
 * it contains information about which of the possible optional data inputs are
 * to be used, and how.
 *
 * The challenge string passed in @challenges contains either one challenge
 * question (in case of one-way authentication) or the correct concatenation of
 * both challenge questions (in case of two-way authentication). It must be
 * NUL-terminated.
 *
 * The output buffer @output_ocra must have room for at least as many digits as
 * specified as part of @ocrasuite, plus one terminating NUL char.
 *
 * Returns: on success, %OATH_OK (zero) is returned, otherwise an error code is
 *   returned.
 *
 * Since: 2.6.0
 **/

int
oath_ocra_generate3 (const char *secret, size_t secret_length,
		     const char *ocrasuite,
		     uint64_t counter, const char *challenges,
		     const char *password_hash,
		     const char *session, time_t now, char *output_ocra)
{
  int rc;
  oath_ocrasuite_t os;
  char *challenges_bin;
  size_t challenges_bin_length;

  rc = parse_ocrasuite (ocrasuite, &os);
  if (rc != OATH_OK)
    return rc;

  if (challenges == NULL)
    return OATH_SUITE_MISMATCH_ERROR;

  challenges_bin = oath_ocra_convert_challenge (os.challenge_type,
						challenges,
						&challenges_bin_length);
  if (challenges_bin == NULL)
    return -1;
  rc =
    oath_ocra_generate_internal (secret, secret_length,
				 counter,
				 challenges_bin,
				 challenges_bin_length,
				 password_hash, session, now,
				 &os, output_ocra);
  free (challenges_bin);
  return rc;
}

/**
 * oath_ocra_validate:
 * @secret: The shared secret string.
 * @secret_length: Length of @secret.
 * @ocrasuite: String with information about used hash algorithms and input.
 * @ocrasuite_length: Length of @ocrasuite.
 * @counter: Counter value, optional (see @ocrasuite).
 * @challenges: Client/server challenge values, byte-array, mandatory.
 * @challenges_length: Length of @challenges.
 * @password_hash: Hashed password value, optional (see @ocrasuite).
 * @session: Static data about current session, optional (see @ocra-suite).
 * @now: Current timestamp, optional (see @ocrasuite).
 * @validate_ocra: OCRA value to validate against.
 *
 * Validates a given OCRA value by generating an OCRA value using the given
 * parameters and comparing the result.
 *
 * Returns: %OATH_OK (zero) on successful validation, an error code otherwise.
 * Since: 2.6.0
 **/
int
oath_ocra_validate (const char *secret, size_t secret_length,
		    const char *ocrasuite,
		    uint64_t counter,
		    const char *challenges,
		    size_t challenges_length,
		    const char *password_hash,
		    const char *session, time_t now,
		    const char *validate_ocra)
{

  int rc;
  char generated_ocra[11];	/* max 10 digits */
  rc = oath_ocra_generate (secret, secret_length,
			   ocrasuite,
			   counter, challenges,
			   challenges_length, password_hash,
			   session, now, generated_ocra);
  if (rc != OATH_OK)
    return rc;
  if (strcmp (generated_ocra, validate_ocra) != 0)
    return OATH_STRCMP_ERROR;
  return OATH_OK;
}

/**
 * oath_ocra_validate2:
 * @secret: The shared secret string.
 * @secret_length: Length of @secret.
 * @ocrasuite: String with information about used hash algorithms and input.
 * @counter: Counter value, optional (see @ocrasuite).
 * @challenges: Array of challenge strings, mandatory
 * @password_hash: Hashed password value, optional (see @ocrasuite).
 * @session: Static data about current session, optional (see @ocra-suite).
 * @now: Current timestamp, optional (see @ocrasuite).
 * @validate_ocra: OCRA value to validate against.
 *
 * Validates a given OCRA value by generating an OCRA value by passing the given
 * parameters to %oath_ocra_generate2 and comparing the result.
 *
 * The string @ocrasuite denotes which mode of OCRA is to be used. Furthermore
 * it contains information about which of the possible optional data inputs are
 * to be used, and how.
 *
 * The challenge strings passed in @challenges are combined into one long
 * challenge string, which is then converted to binary. They must be
 * NUL-terminated.
 *
 * Returns: %OATH_OK (zero) on successful validation, an error code otherwise.
 *
 * Since: 2.6.0
 **/
int
oath_ocra_validate2 (const char *secret, size_t secret_length,
		     const char *ocrasuite,
		     uint64_t counter,
		     const char **challenges, size_t challenges_count,
		     const char *password_hash, const char *session,
		     time_t now, const char *validate_ocra)
{
  int rc;
  char generated_ocra[11];
  rc =
    oath_ocra_generate2 (secret, secret_length, ocrasuite,
			 counter, challenges, challenges_count, password_hash,
			 session, now, generated_ocra);
  if (rc != OATH_OK)
    return rc;
  if (strcmp (generated_ocra, validate_ocra) != 0)
    return OATH_STRCMP_ERROR;
  return OATH_OK;
}

/**
 * oath_ocra_validate3:
 * @secret: The shared secret string.
 * @secret_length: Length of @secret.
 * @ocrasuite: String with information about used hash algorithms and input.
 * @counter: Counter value, optional (see @ocrasuite).
 * @challenges: Challenge string, mandatory
 * @password_hash: Hashed password value, optional (see @ocrasuite).
 * @session: Static data about current session, optional (see @ocra-suite).
 * @now: Current timestamp, optional (see @ocrasuite).
 * @validate_ocra: OCRA value to validate against.
 *
 * Validates a given OCRA value by generating an OCRA value by passing the given
 * parameters to %oath_ocra_generate3 and comparing the result.
 * 
 * The challenge string passed in @challenges contains either one challenge
 * question (in case of one-way authentication) or the correct concatenation of
 * both challenge questions (in case of two-way authentication). It must be
 * NUL-terminated.
 *
 * Returns: %OATH_OK (zero) on successful validation, an error code otherwise.
 *
 * Since: 2.6.0
 **/

int
oath_ocra_validate3 (const char *secret, size_t secret_length,
		     const char *ocrasuite,
		     uint64_t counter, const char *challenges,
		     const char *password_hash,
		     const char *session, time_t now,
		     const char *validate_ocra)
{
  int rc;
  char generated_ocra[11];
  rc =
    oath_ocra_generate3 (secret, secret_length, ocrasuite,
			 counter, challenges, password_hash, session, now,
			 generated_ocra);
  if (rc != OATH_OK)
    return rc;
  if (strcmp (generated_ocra, validate_ocra) != 0)
    return OATH_STRCMP_ERROR;
  return OATH_OK;
}
