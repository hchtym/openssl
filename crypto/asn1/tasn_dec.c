/* tasn_dec.c */
/* Written by Dr Stephen N Henson (shenson@bigfoot.com) for the OpenSSL
 * project 2000.
 */
/* ====================================================================
 * Copyright (c) 2000 The OpenSSL Project.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. All advertising materials mentioning features or use of this
 *    software must display the following acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit. (http://www.OpenSSL.org/)"
 *
 * 4. The names "OpenSSL Toolkit" and "OpenSSL Project" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For written permission, please contact
 *    licensing@OpenSSL.org.
 *
 * 5. Products derived from this software may not be called "OpenSSL"
 *    nor may "OpenSSL" appear in their names without prior written
 *    permission of the OpenSSL Project.
 *
 * 6. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit (http://www.OpenSSL.org/)"
 *
 * THIS SOFTWARE IS PROVIDED BY THE OpenSSL PROJECT ``AS IS'' AND ANY
 * EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE OpenSSL PROJECT OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 * ====================================================================
 *
 * This product includes cryptographic software written by Eric Young
 * (eay@cryptsoft.com).  This product includes software written by Tim
 * Hudson (tjh@cryptsoft.com).
 *
 */


#include <stddef.h>
#include <openssl/asn1.h>
#include <openssl/asn1t.h>
#include <openssl/objects.h>
#include <openssl/buffer.h>
#include <openssl/err.h>

static int asn1_check_eoc(unsigned char **in, long len);
static int asn1_collect(BUF_MEM *buf, unsigned char **in, long len, char inf, int tag, int aclass);
static int collect_data(BUF_MEM *buf, unsigned char **p, long plen);
static int asn1_check_tlen(long *olen, int *otag, unsigned char *oclass, char *inf, char *cst,
			unsigned char **in, long len, int exptag, int expclass, char opt, ASN1_TLC *ctx);
static int asn1_template_ex_d2i(ASN1_VALUE **pval, unsigned char **in, long len, const ASN1_TEMPLATE *tt, char opt, ASN1_TLC *ctx);
static int asn1_template_noexp_d2i(ASN1_VALUE **val, unsigned char **in, long len, const ASN1_TEMPLATE *tt, char opt, ASN1_TLC *ctx);
static int asn1_d2i_ex_primitive(ASN1_VALUE **pval, unsigned char **in, long len,
					int utype, int tag, int aclass, char opt, ASN1_TLC *ctx);

/* Macro to initialize and invalidate the cache */

#define asn1_tlc_clear(c)	if(c) (c)->valid = 0

/* Decode an ASN1 item, this currently behaves just 
 * like a standard 'd2i' function. 'in' points to 
 * a buffer to read the data from, in future we will
 * have more advanced versions that can input data
 * a piece at a time and this will simply be a special
 * case.
 */

ASN1_VALUE *ASN1_item_d2i(ASN1_VALUE **pval, unsigned char **in, long len, const ASN1_ITEM *it)
{
	ASN1_TLC c;
	ASN1_VALUE *ptmpval = NULL;
	if(!pval) pval = &ptmpval;
	asn1_tlc_clear(&c);
	if(ASN1_item_ex_d2i(pval, in, len, it, -1, 0, 0, &c) > 0) 
		return *pval;
	return NULL;
}

int ASN1_template_d2i(ASN1_VALUE **pval, unsigned char **in, long len, const ASN1_TEMPLATE *tt)
{
	ASN1_TLC c;
	asn1_tlc_clear(&c);
	return asn1_template_ex_d2i(pval, in, len, tt, 0, &c);
}


/* Decode an item, taking care of IMPLICIT tagging, if any.
 * If 'opt' set and tag mismatch return -1 to handle OPTIONAL
 */

int ASN1_item_ex_d2i(ASN1_VALUE **pval, unsigned char **in, long len, const ASN1_ITEM *it,
				int tag, int aclass, char opt, ASN1_TLC *ctx)
{
	const ASN1_TEMPLATE *tt, *errtt = NULL;
	const ASN1_COMPAT_FUNCS *cf;
	const ASN1_EXTERN_FUNCS *ef;
	const ASN1_AUX *aux = it->funcs;
	ASN1_aux_cb *asn1_cb;
	unsigned char *p, *q, imphack = 0, oclass;
	char seq_eoc, seq_nolen, cst, isopt;
	int i;
	int otag;
	int ret = 0;
	ASN1_VALUE *pchval, **pchptr, *ptmpval;
	if(!pval) return 0;
	if(aux && aux->asn1_cb) asn1_cb = aux->asn1_cb;
	else asn1_cb = 0;

	switch(it->itype) {

		case ASN1_ITYPE_PRIMITIVE:
		if(it->templates)
			return asn1_template_ex_d2i(pval, in, len, it->templates, opt, ctx);
		return asn1_d2i_ex_primitive(pval, in, len, it->utype, tag, aclass, opt, ctx);
		break;

		case ASN1_ITYPE_MSTRING:
		p = *in;
		/* Just read in tag and class */
		ret = asn1_check_tlen(NULL, &otag, &oclass, NULL, NULL, &p, len, -1, 0, 1, ctx);
		if(!ret) {
			ASN1err(ASN1_F_ASN1_ITEM_EX_D2I, ERR_R_NESTED_ASN1_ERROR);
			goto err;
		} 
		/* Must be UNIVERSAL class */
		if(oclass != V_ASN1_UNIVERSAL) {
			/* If OPTIONAL, assume this is OK */
			if(opt) return -1;
			ASN1err(ASN1_F_ASN1_ITEM_EX_D2I, ASN1_R_MSTRING_NOT_UNIVERSAL);
			goto err;
		} 
		/* Check tag matches bit map */
		if(!(ASN1_tag2bit(otag) & it->utype)) {
			/* If OPTIONAL, assume this is OK */
			if(opt) return -1;
			ASN1err(ASN1_F_ASN1_ITEM_EX_D2I, ASN1_R_MSTRING_WRONG_TAG);
			goto err;
		} 
		return asn1_d2i_ex_primitive(pval, in, len, otag, -1, 0, 1, ctx);

		case ASN1_ITYPE_EXTERN:
		/* Use new style d2i */
		ef = it->funcs;
		return ef->asn1_ex_d2i(pval, in, len, it, tag, aclass, opt, ctx);

		case ASN1_ITYPE_COMPAT:
		/* we must resort to old style evil hackery */
		cf = it->funcs;

		/* If OPTIONAL see if it is there */
		if(opt) {
			int exptag;
			p = *in;
			if(tag == -1) exptag = it->utype;
			else exptag = tag;
			/* Don't care about anything other than presence of expected tag */
			ret = asn1_check_tlen(NULL, NULL, NULL, NULL, NULL, &p, len, exptag, aclass, 1, ctx);
			if(!ret) {
				ASN1err(ASN1_F_ASN1_ITEM_EX_D2I, ERR_R_NESTED_ASN1_ERROR);
				goto err;
			}
			if(ret == -1) return -1;
		}
		/* This is the old style evil hack IMPLICIT handling:
		 * since the underlying code is expecting a tag and
		 * class other than the one present we change the
		 * buffer temporarily then change it back afterwards.
		 * This doesn't and never did work for tags > 30.
		 *
		 * Yes this is *horrible* but it is only needed for
		 * old style d2i which will hopefully not be around
		 * for much longer.
		 * FIXME: should copy the buffer then modify it so
		 * the input buffer can be const: we should *always*
		 * copy because the old style d2i might modify the
		 * buffer.
		 */

		if(tag != -1) {
			p = *in;
			imphack = *p;
			*p = (*p & V_ASN1_CONSTRUCTED) | it->utype;
		}

		ptmpval = cf->asn1_d2i(pval, in, len);

		if(tag != -1) *p = imphack;

		if(ptmpval) return 1;
		ASN1err(ASN1_F_ASN1_ITEM_EX_D2I, ERR_R_NESTED_ASN1_ERROR);
		goto err;


		case ASN1_ITYPE_CHOICE:
		if(asn1_cb && !asn1_cb(ASN1_OP_D2I_PRE, pval, it))
				goto auxerr;
		/* CHOICE type, try each possibility in turn */
		pchval = NULL;
		p = *in;
		for(i = 0, tt=it->templates; i < it->tcount; i++, tt++) {
			/* We mark field as OPTIONAL so its absence
			 * can be recognised.
			 */
			ret = asn1_template_ex_d2i(&pchval, &p, len, tt, 1, ctx);
			/* If field not present, try the next one */
			if(ret == -1) continue;
			/* If positive return, read OK, break loop */
			if(ret > 0) break;
			/* Otherwise must be an ASN1 parsing error */
			errtt = tt;
			ASN1err(ASN1_F_ASN1_ITEM_EX_D2I, ERR_R_NESTED_ASN1_ERROR);
			/* FIXME: note choice type that did this */
			return 0;
		}
		/* Did we fall off the end without reading anything? */
		if(i == it->tcount) {
			/* If OPTIONAL, this is OK */
			if(opt) return -1;
			ASN1err(ASN1_F_ASN1_ITEM_EX_D2I, ASN1_R_NO_MATCHING_CHOICE_TYPE);
			return 0;
		}
		/* Otherwise we got a match, allocate structure and populate it */
		if(!*pval) {
			if(!ASN1_item_ex_new(pval, it)) {
				errtt = tt;
				ASN1err(ASN1_F_ASN1_ITEM_EX_D2I, ERR_R_NESTED_ASN1_ERROR);
				return 0;
			}
		}
		pchptr = asn1_get_field_ptr(pval, tt);
		*pchptr = pchval;
		asn1_set_choice_selector(pval, i, it);
		*in = p;
		if(asn1_cb && !asn1_cb(ASN1_OP_D2I_POST, pval, it))
				goto auxerr;
		return 1;

		case ASN1_ITYPE_SEQUENCE:
		p = *in;
		/* If no IMPLICIT tagging set to SEQUENCE, UNIVERSAL */
		if(tag == -1) {
			tag = V_ASN1_SEQUENCE;
			aclass = V_ASN1_UNIVERSAL;
		}
		/* Get SEQUENCE length and update len, p */
		ret = asn1_check_tlen(&len, NULL, NULL, &seq_eoc, &cst, &p, len, tag, aclass, opt, ctx);
		if(!ret) {
			ASN1err(ASN1_F_ASN1_ITEM_EX_D2I, ERR_R_NESTED_ASN1_ERROR);
			goto err;
		} else if(ret == -1) return -1;
		seq_nolen = seq_eoc;	/* If indefinite we don't do a length check */
		if(!cst) {
			ASN1err(ASN1_F_ASN1_ITEM_EX_D2I, ASN1_R_SEQUENCE_NOT_CONSTRUCTED);
			goto err;
		}

		if(!*pval) {
			if(!ASN1_item_ex_new(pval, it)) {
				ASN1err(ASN1_F_ASN1_ITEM_EX_D2I, ERR_R_NESTED_ASN1_ERROR);
				goto err;
			}
		}
		if(asn1_cb && !asn1_cb(ASN1_OP_D2I_PRE, pval, it))
				goto auxerr;

		/* Get each field entry */
		for(i = 0, tt = it->templates; i < it->tcount; i++, tt++) {
			const ASN1_TEMPLATE *seqtt;
			ASN1_VALUE **pseqval;
			seqtt = asn1_do_adb(pval, tt, 1);
			if(!seqtt) goto err;
			pseqval = asn1_get_field_ptr(pval, seqtt);
			/* Have we ran out of data? */
			if(!len) break;
			q = p;
			if(asn1_check_eoc(&p, len)) {
				if(!seq_eoc) {
					ASN1err(ASN1_F_ASN1_ITEM_EX_D2I, ASN1_R_UNEXPECTED_EOC);
					goto err;
				}
				len -= p - q;
				seq_eoc = 0;
				q = p;
				break;
			}
			/* This determines the OPTIONAL flag value. The field cannot
			 * be omitted if it is the last of a SEQUENCE and there is
			 * still data to be read. This isn't strictly necessary but
			 * it increases efficiency in some cases.
			 */
			if(i == (it->tcount - 1)) isopt = 0;
			else isopt = seqtt->flags & ASN1_TFLG_OPTIONAL;
			/* attempt to read in field, allowing each to be OPTIONAL */
			ret = asn1_template_ex_d2i(pseqval, &p, len, seqtt, isopt, ctx);
			if(!ret) {
				errtt = seqtt;
				goto err;
			} else if(ret == -1) {
				/* OPTIONAL component absent.
				 * Since this is the only place we take any notice of
				 * OPTIONAL this is the only place we need to handle freeing
				 * and zeroing the field.
				 * BOOLEAN is a special case as always and must be set to -1.
				 */
				if(asn1_template_is_bool(seqtt))
					*(ASN1_BOOLEAN *)pseqval = -1;
				else if(*pseqval) {
					ASN1_template_free(pseqval, seqtt);
					*pseqval = NULL;
				}
				continue;
			}
			/* Update length */
			len -= p - q;
		}
		/* Check for EOC if expecting one */
		if(seq_eoc && !asn1_check_eoc(&p, len)) {
			ASN1err(ASN1_F_ASN1_ITEM_EX_D2I, ASN1_R_MISSING_EOC);
			goto err;
		}
		/* Check all data read */
		if(!seq_nolen && len) {
			ASN1err(ASN1_F_ASN1_ITEM_EX_D2I, ASN1_R_SEQUENCE_LENGTH_MISMATCH);
			goto err;
		}

		/* If we get here we've got no more data in the SEQUENCE,
		 * however we may not have read all fields so check all
		 * remaining are OPTIONAL.
		 */
		for(; i < it->tcount; tt++, i++) {
			const ASN1_TEMPLATE *seqtt;
			seqtt = asn1_do_adb(pval, tt, 1);
			if(!seqtt) goto err;
			if(!(seqtt->flags & ASN1_TFLG_OPTIONAL)) {
				errtt = seqtt;
				ASN1err(ASN1_F_ASN1_ITEM_EX_D2I, ASN1_R_FIELD_MISSING);
				goto err;
			}
		}

		*in = p;
		if(asn1_cb && !asn1_cb(ASN1_OP_D2I_POST, pval, it))
				goto auxerr;
		return 1;

		default:
		return 0;
	}
	auxerr:
	ASN1err(ASN1_F_ASN1_ITEM_EX_D2I, ASN1_R_AUX_ERROR);
	err:
	ASN1_item_free(*pval, it);
	*pval = NULL;
	if(errtt) ERR_add_error_data(4, "Field=", errtt->field_name, ", Type=", it->sname);
	else ERR_add_error_data(2, "Type=", it->sname);
	return 0;
}

/* Templates are handled with two separate functions. One handles any EXPLICIT tag and the other handles the
 * rest.
 */

int asn1_template_ex_d2i(ASN1_VALUE **val, unsigned char **in, long inlen, const ASN1_TEMPLATE *tt, char opt, ASN1_TLC *ctx)
{
	int flags, aclass;
	int ret;
	long len;
	unsigned char *p, *q;
	char exp_eoc;
	if(!val) return 0;
	flags = tt->flags;
	aclass = flags & ASN1_TFLG_TAG_CLASS;

	p = *in;

	/* Check if EXPLICIT tag expected */
	if(flags & ASN1_TFLG_EXPTAG) {
		char cst;
		/* Need to work out amount of data available to the inner content and where it
		 * starts: so read in EXPLICIT header to get the info.
		 */
		ret = asn1_check_tlen(&len, NULL, NULL, &exp_eoc, &cst, &p, inlen, tt->tag, aclass, opt, ctx);
		q = p;
		if(!ret) {
			ASN1err(ASN1_F_ASN1_TEMPLATE_EX_D2I, ERR_R_NESTED_ASN1_ERROR);
			return 0;
		} else if(ret == -1) return -1;
		if(!cst) {
			ASN1err(ASN1_F_ASN1_TEMPLATE_EX_D2I, ASN1_R_EXPLICIT_TAG_NOT_CONSTRUCTED);
			return 0;
		}
		/* We've found the field so it can't be OPTIONAL now */
		ret = asn1_template_noexp_d2i(val, &p, len, tt, 0, ctx);
		if(!ret) {
			ASN1err(ASN1_F_ASN1_TEMPLATE_EX_D2I, ERR_R_NESTED_ASN1_ERROR);
			return 0;
		}
		/* We read the field in OK so update length */
		len -= p - q;
		if(exp_eoc) {
			/* If NDEF we must have an EOC here */
			if(!asn1_check_eoc(&p, len)) {
				ASN1err(ASN1_F_ASN1_TEMPLATE_D2I, ASN1_R_MISSING_EOC);
				goto err;
			}
		} else {
			/* Otherwise we must hit the EXPLICIT tag end or its an error */
			if(len) {
				ASN1err(ASN1_F_ASN1_TEMPLATE_D2I, ASN1_R_EXPLICIT_LENGTH_MISMATCH);
				goto err;
			}
		}
	} else 
		return asn1_template_noexp_d2i(val, in, inlen, tt, opt, ctx);

	*in = p;
	return 1;

	err:
	ASN1_template_free(val, tt);
	*val = NULL;
	return 0;
}

static int asn1_template_noexp_d2i(ASN1_VALUE **val, unsigned char **in, long len, const ASN1_TEMPLATE *tt, char opt, ASN1_TLC *ctx)
{
	int flags, aclass;
	int ret;
	unsigned char *p, *q;
	if(!val) return 0;
	flags = tt->flags;
	aclass = flags & ASN1_TFLG_TAG_CLASS;

	p = *in;
	q = p;

	if(flags & ASN1_TFLG_SK_MASK) {
		/* SET OF, SEQUENCE OF */
		int sktag, skaclass;
		char sk_eoc;
		/* First work out expected inner tag value */
		if(flags & ASN1_TFLG_IMPTAG) {
			sktag = tt->tag;
			skaclass = aclass;
		} else {
			skaclass = V_ASN1_UNIVERSAL;
			if(flags & ASN1_TFLG_SET_OF) sktag = V_ASN1_SET;
			else sktag = V_ASN1_SEQUENCE;
		}
		/* Get the tag */
		ret = asn1_check_tlen(&len, NULL, NULL, &sk_eoc, NULL, &p, len, sktag, skaclass, opt, ctx);
		if(!ret) {
			ASN1err(ASN1_F_ASN1_TEMPLATE_EX_D2I, ERR_R_NESTED_ASN1_ERROR);
			return 0;
		} else if(ret == -1) return -1;
		if(!*val) *val = (ASN1_VALUE *)sk_new_null();
		if(!*val) {
			ASN1err(ASN1_F_ASN1_TEMPLATE_EX_D2I, ERR_R_MALLOC_FAILURE);
			goto err;
		}
		/* Read as many items as we can */
		while(len > 0) {
			ASN1_VALUE *skfield;
			q = p;
			/* See if EOC found */
			if(asn1_check_eoc(&p, len)) {
				if(!sk_eoc) {
					ASN1err(ASN1_F_ASN1_TEMPLATE_D2I, ASN1_R_UNEXPECTED_EOC);
					goto err;
				}
				len -= p - q;
				sk_eoc = 0;
				break;
			}
			skfield = NULL;
			if(!ASN1_item_ex_d2i(&skfield, &p, len, tt->item, -1, 0, 0, ctx)) {
				ASN1err(ASN1_F_ASN1_TEMPLATE_D2I, ERR_R_NESTED_ASN1_ERROR);
				goto err;
			}
			len -= p - q;
			if(!sk_push((STACK *)*val, (char *)skfield)) {
				ASN1err(ASN1_F_ASN1_TEMPLATE_D2I, ERR_R_MALLOC_FAILURE);
				goto err;
			}
		}
		if(sk_eoc) {
			ASN1err(ASN1_F_ASN1_TEMPLATE_D2I, ASN1_R_MISSING_EOC);
			goto err;
		}
	} else if(flags & ASN1_TFLG_IMPTAG) {
		/* IMPLICIT tagging */
		ret = ASN1_item_ex_d2i(val, &p, len, tt->item, tt->tag, aclass, opt, ctx);
		if(!ret) {
			ASN1err(ASN1_F_ASN1_TEMPLATE_D2I, ERR_R_NESTED_ASN1_ERROR);
			goto err;
		} else if(ret == -1) return -1;
	} else {
		/* Nothing special */
		ret = ASN1_item_ex_d2i(val, &p, len, tt->item, -1, 0, opt, ctx);
		if(!ret) {
			ASN1err(ASN1_F_ASN1_TEMPLATE_D2I, ERR_R_NESTED_ASN1_ERROR);
			goto err;
		} else if(ret == -1) return -1;
	}

	*in = p;
	return 1;

	err:
	ASN1_template_free(val, tt);
	*val = NULL;
	return 0;
}

static int asn1_d2i_ex_primitive(ASN1_VALUE **pval, unsigned char **in, long inlen, int utype,
						int tag, int aclass, char opt, ASN1_TLC *ctx)
{
	int ret = 0;
	long plen;
	char cst, inf;
	unsigned char *p;
	BUF_MEM buf;
	unsigned char *cont = NULL;
	ASN1_STRING *stmp;
	ASN1_BOOLEAN *tbool;
	long len; 
	if(!pval) {
		ASN1err(ASN1_F_ASN1_D2I_EX_PRIMITIVE, ASN1_R_ILLEGAL_NULL);
		return 0; /* Should never happen */
	}
	if(utype == V_ASN1_ANY) {
		int otag;
		unsigned char oclass;
		ASN1_TYPE *ttmp;
		ASN1_VALUE *anytype;
		if(tag >= 0) {
			ASN1err(ASN1_F_ASN1_D2I_EX_PRIMITIVE, ASN1_R_ILLEGAL_TAGGED_ANY);
			return 0;
		}
		if(opt) {
			ASN1err(ASN1_F_ASN1_D2I_EX_PRIMITIVE, ASN1_R_ILLEGAL_OPTIONAL_ANY);
			return 0;
		}
		p = *in;
		ret = asn1_check_tlen(NULL, &otag, &oclass, NULL, NULL, &p, inlen, -1, 0, 0, ctx);
		if(!ret) {
			ASN1err(ASN1_F_ASN1_D2I_EX_PRIMITIVE, ERR_R_NESTED_ASN1_ERROR);
			return 0;
		}
		if(oclass != V_ASN1_UNIVERSAL) otag = V_ASN1_OTHER;
		anytype = NULL;
		ret = asn1_d2i_ex_primitive(&anytype, in, inlen, otag, -1, 0, 0, ctx);
		if(!ret) {
			ASN1err(ASN1_F_ASN1_D2I_EX_PRIMITIVE, ERR_R_NESTED_ASN1_ERROR);
			return 0;
		}
		if(*pval) ASN1_TYPE_free((ASN1_TYPE *)*pval);
		ttmp = ASN1_TYPE_new();
		*pval = (ASN1_VALUE *)ttmp;
		if(!ttmp) {
			ASN1err(ASN1_F_ASN1_D2I_EX_PRIMITIVE, ERR_R_MALLOC_FAILURE);
			return 0;
		}
		if(otag != V_ASN1_NULL) ttmp->value.ptr = (char *)anytype;
		ttmp->type = otag;
		return 1;

	}
	if(tag == -1) {
		tag = utype;
		aclass = V_ASN1_UNIVERSAL;
	}
	p = *in;
	/* Check header */
	ret = asn1_check_tlen(&plen, NULL, NULL, &inf, &cst, &p, inlen, tag, aclass, opt, ctx);
	if(!ret) {
		ASN1err(ASN1_F_ASN1_D2I_EX_PRIMITIVE, ERR_R_NESTED_ASN1_ERROR);
		return 0;
	} else if(ret == -1) return -1;
	/* SEQUENCE, SET and "OTHER" are left in encoded form */
	if((utype == V_ASN1_SEQUENCE) || (utype == V_ASN1_SET) || (utype == V_ASN1_OTHER)) {
		/* SEQUENCE and SET must be constructed */
		if((utype != V_ASN1_OTHER) && !cst) {
			ASN1err(ASN1_F_ASN1_D2I_EX_PRIMITIVE, ASN1_R_TYPE_NOT_CONSTRUCTED);
			return 0;
		}

		cont = *in;
		/* If indefinite length constructed find the real end */
		if(inf) {
			asn1_collect(NULL, &p, plen, inf, -1, -1);
			len = p - cont;
		} else {
			len = p - cont + plen;
			p += plen;
			buf.data = NULL;
		}
	} else if(cst) {
		buf.length = 0;
		buf.max = 0;
		buf.data = NULL;
		/* Should really check the internal tags are correct but
		 * some things may get this wrong. The relevant specs
		 * say that constructed string types should be OCTET STRINGs
		 * internally irrespective of the type. So instead just check
		 * for UNIVERSAL class and ignore the tag.
		 */
		asn1_collect(&buf, &p, plen, inf, -1, V_ASN1_UNIVERSAL);
		cont = (unsigned char *)buf.data;
		len = buf.length;
	} else {
		cont = p;
		len = plen;
		p += plen;
	}

	switch(utype) {
		case V_ASN1_OBJECT:
		if(!c2i_ASN1_OBJECT((ASN1_OBJECT **)pval, &cont, len)) goto err;
		break;

		case V_ASN1_NULL:
		if(plen) {
			ASN1err(ASN1_F_ASN1_D2I_EX_PRIMITIVE, ASN1_R_NULL_IS_WRONG_LENGTH);
			goto err;
		}
		*pval = (ASN1_VALUE *)1;
		break;

		case V_ASN1_BOOLEAN:
		if(plen != 1) {
			ASN1err(ASN1_F_ASN1_D2I_EX_PRIMITIVE, ASN1_R_BOOLEAN_IS_WRONG_LENGTH);
			goto err;
		}
		tbool = (ASN1_BOOLEAN *)pval;
		*tbool = *cont;
		break;

		case V_ASN1_BIT_STRING:
		if(!c2i_ASN1_BIT_STRING((ASN1_BIT_STRING **)pval, &cont, len)) goto err;
		break;

		case V_ASN1_INTEGER:
		case V_ASN1_NEG_INTEGER:
		case V_ASN1_ENUMERATED:
		case V_ASN1_NEG_ENUMERATED:
		if(!c2i_ASN1_INTEGER((ASN1_INTEGER **)pval, &cont, len)) goto err;
		break;

		case V_ASN1_OCTET_STRING:
		case V_ASN1_NUMERICSTRING:
		case V_ASN1_PRINTABLESTRING:
		case V_ASN1_T61STRING:
		case V_ASN1_VIDEOTEXSTRING:
		case V_ASN1_IA5STRING:
		case V_ASN1_UTCTIME:
		case V_ASN1_GENERALIZEDTIME:
		case V_ASN1_GRAPHICSTRING:
		case V_ASN1_VISIBLESTRING:
		case V_ASN1_GENERALSTRING:
		case V_ASN1_UNIVERSALSTRING:
		case V_ASN1_BMPSTRING:
		case V_ASN1_UTF8STRING:
		/* All based on ASN1_STRING and handled the same */
		if(!*pval) {
			stmp = ASN1_STRING_type_new(utype);
			if(!stmp) {
				ASN1err(ASN1_F_ASN1_D2I_EX_PRIMITIVE, ERR_R_MALLOC_FAILURE);
				goto err;
			}
			*pval = (ASN1_VALUE *)stmp;
		} else {
			stmp = (ASN1_STRING *)*pval;
			stmp->type = utype;
		}
		/* If we've already allocated a buffer use it */
		if(cst) {
			if(stmp->data) OPENSSL_free(stmp->data);
			stmp->data = cont;
			stmp->length = len;
			buf.data = NULL;
		} else {
			if(!ASN1_STRING_set(stmp, cont, len)) {
				ASN1err(ASN1_F_ASN1_D2I_EX_PRIMITIVE, ERR_R_MALLOC_FAILURE);
				ASN1_STRING_free(stmp);	
				*pval = NULL;
				goto err;
			}
		}
		break;

		/* If SEQUENCE, or SET or we don't understand the type
		 * then just collect its encoded form 
		 */

		default:
		case V_ASN1_OTHER:
		case V_ASN1_SET:
		case V_ASN1_SEQUENCE:
		if(!*pval) {
			stmp = ASN1_STRING_new();
			if(!stmp) {
				ASN1err(ASN1_F_ASN1_D2I_EX_PRIMITIVE, ERR_R_MALLOC_FAILURE);
				goto err;
			}
			*pval = (ASN1_VALUE *)stmp;
		} else stmp = (ASN1_STRING *)*pval;
		if(!ASN1_STRING_set(stmp, *in, inlen)) {
			ASN1err(ASN1_F_ASN1_D2I_EX_PRIMITIVE, ERR_R_MALLOC_FAILURE);
			ASN1_STRING_free(stmp);	
			*pval = NULL;
			goto err;
		}
		break;

	}

	*in = p;
	ret = 1;
	err:
	if(cst && buf.data) OPENSSL_free(buf.data);
	return ret;
}

/* This function collects the asn1 data from a constructred string
 * type into a buffer. The values of 'in' and 'len' should refer
 * to the contents of the constructed type and 'inf' should be set
 * if it is indefinite length. If 'buf' is NULL then we just want
 * to find the end of the current structure: useful for indefinite
 * length constructed stuff.
 */

static int asn1_collect(BUF_MEM *buf, unsigned char **in, long len, char inf, int tag, int aclass)
{
	unsigned char *p, *q;
	long plen;
	char cst, ininf;
	p = *in;
	inf &= 1;
	/* If no buffer and not indefinite length constructed just pass over the encoded data */
	if(!buf && !inf) {
		*in += len;
		return 1;
	}
	while(len > 0) {
		q = p;
		/* Check for EOC */
		if(asn1_check_eoc(&p, len)) {
			/* EOC is illegal outside indefinite length constructed form */
			if(!inf) {
				ASN1err(ASN1_F_ASN1_COLLECT, ASN1_R_UNEXPECTED_EOC);
				return 0;
			}
			inf = 0;
			break;
		}
		if(!asn1_check_tlen(&plen, NULL, NULL, &ininf, &cst, &p, len, tag, aclass, 0, NULL)) {
			ASN1err(ASN1_F_ASN1_COLLECT, ERR_R_NESTED_ASN1_ERROR);
			return 0;
		}
		/* If indefinite length constructed update max length */
		if(cst) {
			if(!asn1_collect(buf, &p, plen, ininf, tag, aclass)) return 0;
		} else {
			if(!collect_data(buf, &p, plen)) return 0;
		}
		len -= p - q;
	}
	if(inf) {
		ASN1err(ASN1_F_ASN1_COLLECT, ASN1_R_MISSING_EOC);
		return 0;
	}
	*in = p;
	return 1;
}

static int collect_data(BUF_MEM *buf, unsigned char **p, long plen)
{
		int len;
		if(buf) {
			len = buf->length;
			if(!BUF_MEM_grow(buf, len + plen)) {
				ASN1err(ASN1_F_COLLECT_DATA, ERR_R_MALLOC_FAILURE);
				return 0;
			}
			memcpy(buf->data + len, *p, plen);
		}
		*p += plen;
		return 1;
}

/* Check for ASN1 EOC and swallow it if found */

static int asn1_check_eoc(unsigned char **in, long len)
{
	unsigned char *p;
	if(len < 2) return 0;
	p = *in;
	if(!p[0] && !p[1]) {
		*in += 2;
		return 1;
	}
	return 0;
}

/* Check an ASN1 tag and length: a bit like ASN1_get_object
 * but it sets the length for indefinite length constructed
 * form, we don't know the exact length but we can set an
 * upper bound to the amount of data available minus the
 * header length just read.
 */

static int asn1_check_tlen(long *olen, int *otag, unsigned char *oclass, char *inf, char *cst,
		unsigned char **in, long len, int exptag, int expclass, char opt, ASN1_TLC *ctx)
{
	int i;
	int ptag, pclass;
	long plen;
	unsigned char *p, *q;
	p = *in;
	q = p;

	if(ctx && ctx->valid) {
		i = ctx->ret;
		plen = ctx->plen;
		pclass = ctx->pclass;
		ptag = ctx->ptag;
		p += ctx->hdrlen;
	} else {
		i = ASN1_get_object(&p, &plen, &ptag, &pclass, len);
		if(ctx) {
			ctx->ret = i;
			ctx->plen = plen;
			ctx->pclass = pclass;
			ctx->ptag = ptag;
			ctx->hdrlen = p - q;
			ctx->valid = 1;
		}
	}
		
	if(i & 0x80) {
		ASN1err(ASN1_F_ASN1_CHECK_TLEN, ASN1_R_BAD_OBJECT_HEADER);
		asn1_tlc_clear(ctx);
		return 0;
	}
	if(exptag >= 0) {
		if((exptag != ptag) || (expclass != pclass)) {
			/* If type is OPTIONAL, not an error, but indicate missing
			 * type.
			 */
			if(opt) return -1;
			asn1_tlc_clear(ctx);
			ASN1err(ASN1_F_ASN1_CHECK_TLEN, ASN1_R_WRONG_TAG);
			return 0;
		}
		/* We have a tag and class match, so assume we are going to do something with it */
		asn1_tlc_clear(ctx);
	}
	if(i & 1) plen = len - (p - q);

	if(inf) *inf = i & 1;

	if(cst) *cst = i & V_ASN1_CONSTRUCTED;

	if(olen) *olen = plen;
	if(oclass) *oclass = pclass;
	if(otag) *otag = ptag;

	*in = p;
	return 1;
}
