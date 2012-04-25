/*
 * h235crypto.cxx
 *
 * H.235 crypto engine class.
 *
 * H323Plus library
 *
 * Copyright (c) 2012 Jan Willamowius
 *
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Alternatively, the contents of this file may be used under the terms
 * of the General Public License (the  "GNU License"), in which case the
 * provisions of GNU License are applicable instead of those
 * above. If you wish to allow use of your version of this file only
 * under the terms of the GNU License and not to allow others to use
 * your version of this file under the MPL, indicate your decision by
 * deleting the provisions above and replace them with the notice and
 * other provisions required by the GNU License. If you do not delete
 * the provisions above, a recipient may use your version of this file
 * under either the MPL or the GNU License."
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
 * the License for the specific language governing rights and limitations
 * under the License.
 *
 *
 * The Initial Developer of the Original Code is Jan Willamowius.
 *
 * $Id$
 *
 */

#include <ptlib.h>
#include "openh323buildopts.h"

#ifdef H323_H235

#include "h235/h235crypto.h"
#include <openssl/rand.h>

#include "rtp.h"
#include "h235/h235caps.h"
#include "h235/h2356.h"

#ifdef H323_H235_AES256
const char * OID_AES256 = "2.16.840.1.101.3.4.1.42";
#endif
const char * OID_AES192 = "2.16.840.1.101.3.4.1.22";
const char * OID_AES128 = "2.16.840.1.101.3.4.1.2";

// the IV sequence is always 6 bytes long (2 bytes seq number + 4 bytes timestamp)
const unsigned int IV_SEQUENCE_LEN = 6;


// ciphertext stealing code based on a OpenSSL patch by An-Cheng Huang

int EVP_EncryptUpdate_cts(EVP_CIPHER_CTX *ctx, unsigned char *out, int *outl,
                      const unsigned char *in, int inl)
{
  int bl = ctx->cipher->block_size;
  int leftover = 0;
  OPENSSL_assert(bl <= (int)sizeof(ctx->buf));
  *outl = 0;

  if ((ctx->buf_len + inl) <= bl) {
    /* new plaintext is no more than 1 block */
    /* copy the in data into the buffer and return */
    memcpy(&(ctx->buf[ctx->buf_len]), in, inl);
    ctx->buf_len += inl;
    *outl = 0;
    return 1;
  }

  /* more than 1 block of new plaintext available */
  /* encrypt the previous plaintext, if any */
  if (ctx->final_used) {
    if (!(ctx->cipher->do_cipher(ctx, out, ctx->final, bl))) {
      return 0;
    }
    out += bl;
    *outl += bl;
    ctx->final_used = 0;
  }

  /* we already know ctx->buf_len + inl must be > bl */
  memcpy(&(ctx->buf[ctx->buf_len]), in, (bl - ctx->buf_len));
  in += (bl - ctx->buf_len);
  inl -= (bl - ctx->buf_len);
  ctx->buf_len = bl;

  if (inl <= bl) {
    memcpy(ctx->final, ctx->buf, bl);
    ctx->final_used = 1;
    memcpy(ctx->buf, in, inl);
    ctx->buf_len = inl;
    return 1;
  } else {
    if (!(ctx->cipher->do_cipher(ctx, out, ctx->buf, bl))) {
      return 0;
    }
    out += bl;
    *outl += bl;
    ctx->buf_len = 0;

    leftover = inl & ctx->block_mask;
    if (leftover) {
      inl -= (bl + leftover);
      memcpy(ctx->buf, &(in[(inl + bl)]), leftover);
      ctx->buf_len = leftover;
    } else {
      inl -= (2 * bl);
      memcpy(ctx->buf, &(in[(inl + bl)]), bl);
      ctx->buf_len = bl;
    }
    memcpy(ctx->final, &(in[inl]), bl);
    ctx->final_used = 1;
    if (!(ctx->cipher->do_cipher(ctx, out, in, inl))) {
      return 0;
    }
    out += inl;
    *outl += inl;
  }

  return 1;
}

int EVP_EncryptFinal_cts(EVP_CIPHER_CTX *ctx, unsigned char *out, int *outl)
{
  unsigned char tmp[EVP_MAX_BLOCK_LENGTH];
  int bl = ctx->cipher->block_size;
  int leftover = 0;
  *outl = 0;

  if (!ctx->final_used) {
    PTRACE(1, "H235\tCTS Error: expecting previous ciphertext");
    return 0;
  }
  if (ctx->buf_len == 0) {
    PTRACE(1, "H235\tCTS Error: expecting previous plaintext");
    return 0;
  }

  /* handle leftover bytes */
  leftover = ctx->buf_len;

  switch (EVP_CIPHER_CTX_mode(ctx)) {
  case EVP_CIPH_ECB_MODE: {
    /* encrypt => C_{n} plus C' */
    if (!(ctx->cipher->do_cipher(ctx, tmp, ctx->final, bl))) {
      return 0;
    }

    /* P_n plus C' */
    memcpy(&(ctx->buf[leftover]), &(tmp[leftover]), (bl - leftover));
    /* encrypt => C_{n-1} */
    if (!(ctx->cipher->do_cipher(ctx, out, ctx->buf, bl))) {
      return 0;
    }

    memcpy((out + bl), tmp, leftover);
    *outl += (bl + leftover);
    return 1;
  }
  case EVP_CIPH_CBC_MODE: {
    /* encrypt => C_{n} plus C' */
    if (!(ctx->cipher->do_cipher(ctx, tmp, ctx->final, bl))) {
      return 0;
    }

    /* P_n plus 0s */
    memset(&(ctx->buf[leftover]), 0, (bl - leftover));

    /* note that in cbc encryption, plaintext will be xor'ed with the previous
     * ciphertext, which is what we want.
     */
    /* encrypt => C_{n-1} */
    if (!(ctx->cipher->do_cipher(ctx, out, ctx->buf, bl))) {
      return 0;
    }

    memcpy((out + bl), tmp, leftover);
    *outl += (bl + leftover);
    return 1;
  }
  default:
    PTRACE(1, "H235\tCTS Error: unsupported mode");
    return 0;
  }
  return 0;
}

int EVP_DecryptUpdate_cts(EVP_CIPHER_CTX *ctx, unsigned char *out, int *outl,
                      const unsigned char *in, int inl)
{
  return EVP_EncryptUpdate_cts(ctx, out, outl, in, inl);
}

int EVP_DecryptFinal_cts(EVP_CIPHER_CTX *ctx, unsigned char *out, int *outl)
{
  unsigned char tmp[EVP_MAX_BLOCK_LENGTH];
  int bl = ctx->cipher->block_size;
  int leftover = 0;
  *outl = 0;

  if (!ctx->final_used) {
    PTRACE(1, "H235\tCTS Error: expecting previous ciphertext");
    return 0;
  }
  if (ctx->buf_len == 0) {
    PTRACE(1, "H235\tCTS Error: expecting previous ciphertext");
    return 0;
  }

  /* handle leftover bytes */
  leftover = ctx->buf_len;

  switch (EVP_CIPHER_CTX_mode(ctx)) {
  case EVP_CIPH_ECB_MODE: {
    /* decrypt => P_n plus C' */
    if (!(ctx->cipher->do_cipher(ctx, tmp, ctx->final, bl))) {
      return 0;
    }

    /* C_n plus C' */
    memcpy(&(ctx->buf[leftover]), &(tmp[leftover]), (bl - leftover));
    /* decrypt => P_{n-1} */
    if (!(ctx->cipher->do_cipher(ctx, out, ctx->buf, bl))) {
      return 0;
    }

    memcpy((out + bl), tmp, leftover);
    *outl += (bl + leftover);
    return 1;
  }
  case EVP_CIPH_CBC_MODE: {
    int i = 0;
    unsigned char C_n_minus_2[EVP_MAX_BLOCK_LENGTH];

    memcpy(C_n_minus_2, ctx->iv, bl);

    /* C_n plus 0s in ctx->buf */
    memset(&(ctx->buf[leftover]), 0, (bl - leftover));

    /* ctx->final is C_{n-1} */
    /* decrypt => (P_n plus C')'' */
    if (!(ctx->cipher->do_cipher(ctx, tmp, ctx->final, bl))) {
      return 0;
    }
    /* XOR'ed with C_{n-2} => (P_n plus C')' */
    for (i = 0; i < bl; i++) {
      tmp[i] = tmp[i] ^ C_n_minus_2[i];
    }
    /* XOR'ed with (C_n plus 0s) => P_n plus C' */
    for (i = 0; i < bl; i++) {
      tmp[i] = tmp[i] ^ ctx->buf[i];
    }

    /* C_n plus C' in ctx->buf */
    memcpy(&(ctx->buf[leftover]), &(tmp[leftover]), (bl - leftover));
    /* decrypt => P_{n-1}'' */
    if (!(ctx->cipher->do_cipher(ctx, out, ctx->buf, bl))) {
      return 0;
    }
    /* XOR'ed with C_{n-1} => P_{n-1}' */
    for (i = 0; i < bl; i++) {
      out[i] = out[i] ^ ctx->final[i];
    }
    /* XOR'ed with C_{n-2} => P_{n-1} */
    for (i = 0; i < bl; i++) {
      out[i] = out[i] ^ C_n_minus_2[i];
    }

    memcpy((out + bl), tmp, leftover);
    *outl += (bl + leftover);
    return 1;
  }
  default:
    PTRACE(1, "H235\tCTS Error: unsupported mode");
    return 0;
  }
  return 0;
}

int EVP_DecryptFinal_relaxed(EVP_CIPHER_CTX *ctx, unsigned char *out, int *outl)
	{
	int i,n;
	unsigned int b;
	*outl=0;

	b=ctx->cipher->block_size;
	if (ctx->flags & EVP_CIPH_NO_PADDING)
		{
		if(ctx->buf_len)
			{
			PTRACE(1, "H235\tDecrypt error: data not a multiple of block length");
			return 0;
			}
		*outl = 0;
		return 1;
		}
	if (b > 1)
		{
		if (ctx->buf_len || !ctx->final_used)
			{
			PTRACE(1, "H235\tDecrypt error: wrong final block length");
			return(0);
			}
		OPENSSL_assert(b <= sizeof ctx->final);
		n=ctx->final[b-1];
		if (n == 0 || n > (int)b)
			{
			PTRACE(1, "H235\tDecrypt error: bad decrypt");
			return(0);
			}
        // Polycom endpoints (eg. m100 and PVX) don't fill the padding propperly, so we have to disable this check
/*
		for (i=0; i<n; i++)
			{
			if (ctx->final[--b] != n)
				{
			    PTRACE(1, "H235\tDecrypt error: incorrect padding");
				return(0);
				}
			}
*/
		n=ctx->cipher->block_size-n;
		for (i=0; i<n; i++)
			out[i]=ctx->final[i];
		*outl=n;
		}
	else
		*outl=0;
	return(1);
	}

H235CryptoEngine::H235CryptoEngine(const PString & algorithmOID)
{
    m_algorithmOID = algorithmOID;
}

H235CryptoEngine::H235CryptoEngine(const PString & algorithmOID, const PBYTEArray & key)
{
    m_algorithmOID = algorithmOID;
    SetKey(key);
}

H235CryptoEngine::~H235CryptoEngine()
{
    EVP_CIPHER_CTX_cleanup(&m_encryptCtx);
    EVP_CIPHER_CTX_cleanup(&m_decryptCtx);
}

void H235CryptoEngine::SetKey(PBYTEArray key)
{
    const EVP_CIPHER * cipher = NULL;

    if (m_algorithmOID == OID_AES128) {
        cipher = EVP_aes_128_cbc();
    } else if (m_algorithmOID == OID_AES192) {
        cipher = EVP_aes_192_cbc();
#ifdef H323_H235_AES256
    } else if (m_algorithmOID == OID_AES256) {
        cipher = EVP_aes_256_cbc();
#endif
    } else {
        PTRACE(1, "Unsupported algorithm " << m_algorithmOID);
        return;
    }
  
    EVP_CIPHER_CTX_init(&m_encryptCtx);
    EVP_EncryptInit_ex(&m_encryptCtx, cipher, NULL, key.GetPointer(), NULL);
    EVP_CIPHER_CTX_init(&m_decryptCtx);
    EVP_DecryptInit_ex(&m_decryptCtx, cipher, NULL, key.GetPointer(), NULL);
}

void H235CryptoEngine::SetIV(unsigned char * iv, unsigned char * ivSequence, unsigned ivLen)
{
    // fill iv by repeating ivSequence until block size is reached
    if (ivSequence) {
        for (unsigned i = 0; i < (ivLen / IV_SEQUENCE_LEN); i++) {
            memcpy(iv + (i * IV_SEQUENCE_LEN), ivSequence, IV_SEQUENCE_LEN);
        }
        // copy partial ivSequence at end
        if (ivLen % IV_SEQUENCE_LEN > 0) {
            memcpy(iv + ivLen - (ivLen % IV_SEQUENCE_LEN), ivSequence, ivLen % IV_SEQUENCE_LEN);
        }
    } else {
        memset(iv, 0, ivLen);
    }
}

PBYTEArray H235CryptoEngine::Encrypt(const PBYTEArray & _data, unsigned char * ivSequence, bool & rtpPadding)
{
    PBYTEArray & data = *(PRemoveConst(PBYTEArray, &_data));
    unsigned char iv[EVP_MAX_IV_LENGTH];

    // max ciphertext len for a n bytes of plaintext is n + BLOCK_SIZE -1 bytes
    int ciphertext_len = data.GetSize() + EVP_CIPHER_CTX_block_size(&m_encryptCtx);
    int final_len = 0;
    PBYTEArray ciphertext(ciphertext_len);

    SetIV(iv, ivSequence, EVP_CIPHER_CTX_iv_length(&m_encryptCtx));
    EVP_EncryptInit_ex(&m_encryptCtx, NULL, NULL, NULL, iv);

	rtpPadding = (data.GetSize() < EVP_CIPHER_CTX_block_size(&m_encryptCtx));
    EVP_CIPHER_CTX_set_padding(&m_encryptCtx, rtpPadding ? 1 : 0);

    if (!rtpPadding && (data.GetSize() % EVP_CIPHER_CTX_block_size(&m_encryptCtx) > 0)) {
        // use cyphertext stealing
    	if (!EVP_EncryptUpdate_cts(&m_encryptCtx, ciphertext.GetPointer(), &ciphertext_len, data.GetPointer(), data.GetSize())) {
			PTRACE(1, "H235\tEVP_EncryptUpdate_cts() failed");
		}
       	if (!EVP_EncryptFinal_cts(&m_encryptCtx, ciphertext.GetPointer() + ciphertext_len, &final_len)) {
			PTRACE(1, "H235\tEVP_EncryptFinal_cts() failed");
		}
	} else {
    	/* update ciphertext, ciphertext_len is filled with the length of ciphertext generated,
    	 *len is the size of plaintext in bytes */
    	if (!EVP_EncryptUpdate(&m_encryptCtx, ciphertext.GetPointer(), &ciphertext_len, data.GetPointer(), data.GetSize())) {
			PTRACE(1, "H235\tEVP_EncryptUpdate() failed");
		}

	   	// update ciphertext with the final remaining bytes, if any use RTP padding
      	if (!EVP_EncryptFinal_ex(&m_encryptCtx, ciphertext.GetPointer() + ciphertext_len, &final_len)) {
			PTRACE(1, "H235\tEVP_EncryptFinal_ex() failed");
		}
    }

    ciphertext.SetSize(ciphertext_len + final_len);
    return ciphertext;
}

PBYTEArray H235CryptoEngine::Decrypt(const PBYTEArray & _data, unsigned char * ivSequence, bool & rtpPadding)
{
    PBYTEArray & data = *(PRemoveConst(PBYTEArray, &_data));
    unsigned char iv[EVP_MAX_IV_LENGTH];

    /* plaintext will always be equal to or lesser than length of ciphertext*/
    int plaintext_len = data.GetSize();
    int final_len = 0;
    PBYTEArray plaintext(plaintext_len);
  
    SetIV(iv, ivSequence, EVP_CIPHER_CTX_iv_length(&m_decryptCtx));
    EVP_DecryptInit_ex(&m_decryptCtx, NULL, NULL, NULL, iv);

    EVP_CIPHER_CTX_set_padding(&m_decryptCtx, rtpPadding ? 1 : 0);

    if (!rtpPadding && data.GetSize() % EVP_CIPHER_CTX_block_size(&m_decryptCtx) > 0) {
        // use cyphertext stealing
    	if (!EVP_DecryptUpdate_cts(&m_decryptCtx, plaintext.GetPointer(), &plaintext_len, data.GetPointer(), data.GetSize())) {
        	PTRACE(1, "H235\tEVP_DecryptUpdate_cts() failed");
    	}
    	if(!EVP_DecryptFinal_cts(&m_decryptCtx, plaintext.GetPointer() + plaintext_len, &final_len)) {
        	PTRACE(1, "H235\tEVP_DecryptFinal_cts() failed");
    	}
    } else {
    	if (!EVP_DecryptUpdate(&m_decryptCtx, plaintext.GetPointer(), &plaintext_len, data.GetPointer(), data.GetSize())) {
        	PTRACE(1, "H235\tEVP_DecryptUpdate() failed");
    	}
    	if (!EVP_DecryptFinal_relaxed(&m_decryptCtx, plaintext.GetPointer() + plaintext_len, &final_len)) {
        	PTRACE(1, "H235\tEVP_DecryptFinal_ex() failed - incorrect padding ?");
    	}
    }

    plaintext.SetSize(plaintext_len + final_len);
    return plaintext;
}

PBYTEArray H235CryptoEngine::GenerateRandomKey()
{
    PBYTEArray result = GenerateRandomKey(m_algorithmOID);    
    SetKey(result);
    return result;
}

PBYTEArray H235CryptoEngine::GenerateRandomKey(const PString & algorithmOID)
{
    PBYTEArray key;

    if (algorithmOID == OID_AES128) {
        key.SetSize(16);
    } else if (m_algorithmOID == OID_AES192) {
        key.SetSize(24);
#ifdef H323_H235_AES256
    } else if (m_algorithmOID == OID_AES256) {
        key.SetSize(32);
#endif
    } else {
        PTRACE(1, "Unsupported algorithm " << algorithmOID);
        return key;
    }
    RAND_bytes(key.GetPointer(), key.GetSize());

    return key;
}

///////////////////////////////////////////////////////////////////////////////////

H235Session::H235Session(H235Capabilities * caps, const PString & oidAlgorithm)
: m_dh(*caps->GetDiffieHellMan()), m_context(oidAlgorithm), m_dhcontext(oidAlgorithm), 
  m_isInitialised(false), m_isMaster(false), m_dhSessionkey(0), m_crytoMasterKey(0)
{

}

H235Session::~H235Session()
{

}

void H235Session::EncodeMediaKey(PBYTEArray & key)
{
    PTRACE(4, "H235Key\tEncode plain media key: " << endl << hex << m_crytoMasterKey); 

    bool rtpPadding = false;
    key = m_dhcontext.Encrypt(m_crytoMasterKey, NULL, rtpPadding);

    PTRACE(4, "H235Key\tEncrypted key:" << endl << hex << key);
}

void H235Session::DecodeMediaKey(PBYTEArray & key)
{
    PTRACE(4, "H235Key\tH235v3 encrypted key received, size=" << key.GetSize() << endl << hex << key);

    bool rtpPadding = false;
    m_crytoMasterKey = m_dhcontext.Decrypt(key, NULL, rtpPadding);
    m_context.SetKey(m_crytoMasterKey);

    PTRACE(4, "H235Key\tH235v3 key decrypted, size= " << m_crytoMasterKey.GetSize() << endl << hex << m_crytoMasterKey);
}

PBoolean H235Session::IsActive()
{
    return !IsInitialised();
}

PBoolean H235Session::IsInitialised() 
{ 
    return m_isInitialised; 
}

PBoolean H235Session::CreateSession(PBoolean isMaster)
{
    m_isMaster = isMaster;
    m_isInitialised = true;

    m_dh.ComputeSessionKey(m_dhSessionkey);
    m_dhcontext.SetKey(m_dhSessionkey);

    if (m_isMaster) 
        m_crytoMasterKey = m_context.GenerateRandomKey();

    return true;
}

PBoolean H235Session::ReadFrame(DWORD & /*rtpTimestamp*/, RTP_DataFrame & frame)
{
    //WORD m_ivSequence = frame.GetSequenceNumber(); // TODO: fix ivSequence
    PBoolean m_padding = frame.GetPadding();
    PBYTEArray buffer(frame.GetPayloadPtr(),frame.GetPayloadSize());
    buffer = m_context.Decrypt(buffer, NULL, m_padding);
    frame.SetPayloadSize(buffer.GetSize());
    memcpy(frame.GetPayloadPtr(),buffer.GetPointer(), buffer.GetSize());
    buffer.SetSize(0);
    return true;
}

PBoolean H235Session::WriteFrame(RTP_DataFrame & frame)
{
    //WORD m_ivSequence = frame.GetSequenceNumber(); // TODO: fix ivSequence
    PBoolean m_padding = frame.GetPadding();
    PBYTEArray buffer(frame.GetPayloadPtr(),frame.GetPayloadSize());
    buffer = m_context.Encrypt(buffer, NULL, m_padding);
    frame.SetPayloadSize(buffer.GetSize());
    memcpy(frame.GetPayloadPtr(),buffer.GetPointer(), buffer.GetSize());
    buffer.SetSize(0);
    return true;
}

#endif
