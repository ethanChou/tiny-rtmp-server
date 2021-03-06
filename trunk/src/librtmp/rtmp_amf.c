
/*
 * CopyLeft (C) nie950@gmail.com
 */

#include "librtmp_in.h"

#include "rtmp_amf.h"
#include "rtmp_bytes.h"
#include "rtmp_link.h"

typedef struct amf_prop_s   amf_prop_t;

typedef struct amf_data_s {
    uint8_t          type;
    union {
        uint8_t      b_val;
        double       n_val;

        struct {
            char    *s_val;
            int      s_len;
        };
        
        link_t      *l_val;
    };
}amf_t;

struct amf_prop_s {
    link_t      link;
    char        name[64];
    amf_data_t *data;
};

static amf_data_t* _amf_new_bool(uint8_t b);
static amf_data_t* _amf_new_string(const char *s,int len);
static amf_data_t* _amf_new_number(double n);
static amf_data_t* _amf_new_date(double n,uint16_t u);
static amf_data_t* _amf_new_object();
static amf_data_t* _amf_new_ecma_object();
static amf_data_t* _amf_new_null();
static amf_data_t* _amf_new_object_end();
static amf_data_t* _amf_new_undefined();

static amf_t * amf_decode_in(const char* buf,int *len);
static int amf_encode_in(amf_data_t * data,char* buf,int *len);

static int amf_encode_in_object(amf_t*,char* buf,int *len);
static amf_t* amf_decode_in_object(const char* buf,int *len);

static void *amf_default_malloc(size_t size,void *u);
static void amf_default_free(void *p,void *u);

static void *(*amf_default_malloc_rt)(size_t size,void *u) 
    = amf_default_malloc;

static void (*amf_default_free_rt)(void *p,void *u) 
    = amf_default_free;

static void *_amf_default_u_ = NULL;

#define AMF_malloc(x) amf_default_malloc_rt(x,_amf_default_u_)
#define AMF_free(x)   amf_default_free_rt(x,_amf_default_u_)

void amf_set_allocator(amf_malloc m,amf_free f,void *u)
{
    if (!m || !f) {

        amf_default_malloc_rt = amf_default_malloc;
        amf_default_free_rt = amf_default_free;
        _amf_default_u_ = NULL;

        return ;
    }

    amf_default_malloc_rt = m;
    amf_default_free_rt = f;
    _amf_default_u_ = u;
}

static void *amf_default_malloc(size_t size,void *u)
{
    return malloc(size);
}

static void amf_default_free(void *p,void *u)
{
    if (p != NULL) {
        free(p);
    }
}

uint8_t amf_get_data_type(amf_data_t * data)
{
    const amf_t * d;
    d = (amf_t *)data;

    if (d) {
        return d->type;
    }

    return amf_invalid;
}

amf_data_t* amf_new(uint8_t type,...)
{
    va_list ap;
    amf_data_t *d;
    amf_t tmp;
    int slen;
    uint32_t tz;

    va_start(ap, type);

    switch (type) {
    case amf_null:
        d = _amf_new_null();
        break;

    case amf_object_end:
        d = _amf_new_object_end();
        break;

    case amf_ecma_array:
        d = _amf_new_ecma_object();
        break;

    case amf_object:
        d = _amf_new_object();
        break;

    case amf_boolean:
        tmp.b_val = va_arg(ap,uint32_t);
        d = _amf_new_bool(tmp.b_val);
        break;

    case amf_string:
        tmp.s_val = va_arg(ap,char*);
        slen = va_arg(ap,int32_t);
        if (slen == 0) {
            slen = strlen(tmp.s_val);
        }
        d = _amf_new_string(tmp.s_val,slen);
        break;

    case amf_number:
        tmp.n_val = va_arg(ap,double);
        d = _amf_new_number(tmp.n_val);
        break;

    case amf_date:
        tmp.n_val = va_arg(ap,double);
        tz = va_arg(ap,uint32_t);
        d = _amf_new_date(tmp.n_val,tz);
        break;

    default:
        d = 0;
        break;
    }

    va_end(ap);

    return d;
}

static amf_data_t* _amf_new_bool(uint8_t b)
{
    amf_t * d;

    d = AMF_malloc(sizeof(amf_t));
    
    if (d) {
        d->type = amf_boolean;
        d->b_val = b?1:0;

        return (amf_data_t*)d;
    }

    return NULL;
}

static amf_data_t* _amf_new_string(const char *s,int len)
{
    amf_t * d;

    if ((s == NULL) || (len <= 0)) {
        return NULL;
    }

    d = AMF_malloc(sizeof(amf_t)+len+1);

    if (d) {
        d->type = amf_string;
        if (len > 0xffff) {
            d->type = amf_long_string;
        }

        d->s_val = (char*)d + sizeof(amf_t);

        memcpy(d->s_val,s,len);

        d->s_val[len] = '\0';
        d->s_len      = len;

        return (amf_data_t*)d;
    }

    return NULL;
}

static amf_data_t* _amf_new_number(double n)
{
    amf_t * d;

    d = AMF_malloc(sizeof(amf_t));

    if (d) {
        d->type = amf_number;
        d->n_val = n;

        return (amf_data_t*)d;
    }

    return NULL;

}

static amf_data_t* _amf_new_date(double n,uint16_t u)
{
    amf_t * d;
    char * tz;

    d = AMF_malloc(sizeof(amf_t) + sizeof(uint16_t));

    if (d) {
        d->type = amf_date;
        d->n_val = n;

        tz = (char*)d + sizeof(amf_t);
        ulong_make_byte2(tz,u);
        
        return (amf_data_t*)d;
    }

    return NULL;
}

static amf_data_t* _amf_new_object()
{
    amf_t *d,*e;

    e = (amf_t*)_amf_new_object_end();
    d = AMF_malloc(sizeof(amf_t)+sizeof(list_t));

    if (d) {
        d->type = amf_object;
        d->l_val = (link_t*)((char*)d + sizeof(amf_t));

        /*init props list*/
        list_init((link_t*)d->l_val);

        amf_put_prop((amf_data_t*)d,"",(amf_data_t*)e);

        return (amf_data_t*)d;
    }

    amf_free_data((amf_data_t*)e);
    return NULL;
}

static amf_data_t* _amf_new_ecma_object()
{
    amf_t *d,*e;

    e = (amf_t*)_amf_new_object_end();
    d = AMF_malloc(sizeof(amf_t)+sizeof(list_t));

    if (d) {
        d->type = amf_ecma_array;
        d->l_val = (link_t*)((char*)d + sizeof(amf_t));

        /*init props list*/
        list_init((list_t*)d->l_val);

        amf_put_prop((amf_data_t*)d,"",(amf_data_t*)e);

        return (amf_data_t*)d;
    }

    amf_free_data((amf_data_t*)e);
    return NULL;
}

static amf_data_t* _amf_new_null()
{
    amf_t *d;

    d = AMF_malloc(sizeof(amf_t));

    if (d) {
        d->type = amf_null;
        return (amf_data_t*)d;
    }

    return NULL;
}

static amf_data_t* _amf_new_undefined()
{
    amf_t *d;

    d = AMF_malloc(sizeof(amf_t));

    if (d) {
        d->type = amf_undefined;
        return (amf_data_t*)d;
    }

    return NULL;
}

static amf_data_t* _amf_new_object_end()
{
    amf_t * d;

    d = AMF_malloc(sizeof(amf_t));

    if (d) {
        d->type = amf_object_end;

        return (amf_data_t*)d;
    }

    return NULL;
}

void amf_free_props(amf_data_t * data)
{
    link_t *h,*n,*t;
    amf_t  *d;
    amf_prop_t *prop;

    d = (amf_t *)data;

    if ((d == NULL) || (d->type != amf_object)) {
        return;
    }

    h = (list_t*)d->l_val;
    t = h->next;

    while (t != h) {
        n = t->next;

        prop = struct_entry(t,amf_prop_t,link);

        amf_free_data(prop->data);

        AMF_free(prop);

        t = n;
    }

    return ;
}

void amf_free_data(amf_data_t * data)
{
    amf_t * d;

    d = (amf_t *)data;
    if (d == NULL) {
        return ;
    }

    switch (d->type) {
    case amf_ecma_array:
    case amf_object:
    
        /*free props*/
        amf_free_props((amf_data_t*)d);

        AMF_free(d);
        break;
    default:
        AMF_free(d);
        break;
    }
}

int amf_put_prop(amf_data_t *obj,const char* name,const amf_data_t *data)
{
    link_t *h,*n;
    amf_prop_t *prop;
    int rc;

    if (!obj ||!name || !data) {
        return -1;
    }

    /*name:""  means object_end */
    if ((name[0] == '\0') 
     && (((amf_t *)data)->type != amf_object_end)) 
    {
        return -1;
    }

    if ((((amf_t *)obj)->type != amf_object) 
      && ((amf_t *)obj)->type != amf_ecma_array)
    {
        return -1;
    }

    h = ((amf_t *)obj)->l_val;
    n = h->next;
    while (n != h) {
        prop = struct_entry(n,amf_prop_t,link);

        rc = strcmp(name,prop->name);
        if (rc == 0) {
            return -1;
        }

        if (((amf_t *)(prop->data))->type == amf_object_end) {
            break;
        }
        n = n->next;
    }

    prop = AMF_malloc(sizeof(amf_prop_t));
    if (prop == NULL) {
        return -1;
    }

    prop->name[sizeof(prop->name)-1] = '\0';
    prop->data = (amf_data_t *)data;
    strncpy(prop->name,name,sizeof(prop->name)-1);

    list_insert_tail(n,&prop->link);

    return 0;
}

amf_data_t* amf_get_prop(amf_data_t *obj,const char* name)
{
    amf_t *d;
    link_t *h,*n;
    amf_prop_t *prop;
    int rc;

    if (!obj ||!name) {
        return 0;
    }

    d = (amf_t *)obj;
    if (d->type != amf_object) {
        return 0;
    }

    h = d->l_val;
    n = h->next;
    while (n != h) {
        prop = struct_entry(n,amf_prop_t,link);

        rc = strcmp(name,prop->name);
        if (rc == 0) {
            return prop->data;
        }

        n = n->next;
    }

    return 0;
}

static amf_t* amf_decode_in(const char* buf,int *len)
{
    amf_t *     data;
    amf_t       tmp;
    uint16_t    tz;
    int16_t     slen;
    int32_t     slen_l;
    int         last;
    uint64_t    u64;

    if (*len <= 0) {
        return 0;
    }

    tmp.type = (uint8_t)buf[0];

    (*len)--;
    buf++;
    data = 0;

    switch (tmp.type) {
    case amf_number:
        if (*len < 8) {
            return 0;
        }

        u64 = byte_make_ulong8(buf);
        memcpy(&tmp.n_val,&u64,sizeof(tmp.n_val));

        buf += 8;
        (*len) -= 8;

        data = (amf_t*)_amf_new_number(tmp.n_val);

        break;

    case amf_boolean:
        if (*len < 1) {
            return 0;
        }

        tmp.b_val = (uint8_t)buf[0];
        (*len)--;
        buf++;
        
        data = (amf_t*)_amf_new_bool(tmp.b_val);
        
        break;

    case amf_string:
        if (*len < 2) {
            return 0;
        }

        slen = byte_make_ulong2(buf);
        (*len) -= 2;
        buf += 2;

        if (*len < slen) {
            return 0;
        }

        data = (amf_t*)_amf_new_string(buf,slen);
        
        (*len) -= slen;
        buf += slen;

        break;

    case amf_date:
        if (*len < 8 + 2) {
            return 0;
        }

        u64 = byte_make_ulong8(buf);
        memcpy(&tmp.n_val,&u64,sizeof(tmp.n_val));

        buf += 8;
        (*len) -= 8;

        tz = byte_make_ulong2(buf);
        buf += 2;
        (*len) -= 2;

        data = (amf_t*)_amf_new_date(tmp.n_val,tz);

        break;

    case amf_long_string:
        if (*len < 4) {
            return 0;
        }

        slen_l = byte_make_ulong4(buf);
        (*len) -= 4;
        buf += 4;

        if (*len < slen_l) {
            return 0;
        }

        data = (amf_t*)_amf_new_string(buf,slen_l);

        (*len) -= slen_l;
        buf += slen_l;

        break;

    case amf_avmplus_object:
        
        break;

    case amf_ecma_array:
        
        if (*len < 4) {
            return 0;
        }
        /*ignore count*/
        (*len) -= 4;  
        buf += 4;

        /*no break*/

    case amf_object:

        last = *len;
        data = amf_decode_in_object(buf,len);
        if (data) {
            buf += last - *len;
        }

        break;

    case amf_null:
        data = (amf_t *)_amf_new_null();
        break;

    case amf_object_end:
        data = (amf_t *)_amf_new_object_end();
        break;

    case amf_undefined:
        data = (amf_t *)_amf_new_undefined();
        break;

    default:
        return 0;
    }

    return data;
}

static int amf_encode_in_object(amf_t * d,char* buf,int *len)
{
    const link_t *t,*n;
    amf_prop_t *prop;
    int16_t slen;
    int last,rc;

    t = d->l_val;
    n = t->next;

    prop = 0;

    /*write prop*/
    while (n != d->l_val) {
        prop = struct_entry(n,amf_prop_t,link);

        slen = strlen (prop->name);

        if (*len <= (2 + slen)) {
            break;
        }

        ulong_make_byte2(buf,slen);
        buf += 2;
        (*len) -= 2;

        memcpy(buf,prop->name,slen);
        buf += slen;
        (*len) -= slen;

        last = (*len);

        rc = amf_encode_in(prop->data,buf,len);
        if (rc == -1) {
            return -1;
        }
        buf += last - *len;

        n = n->next;
    }

    if (!prop || !prop->data 
        || ((amf_t*)(prop->data))->type != amf_object_end) 
    {
        return -1;
    }

    return (*len);
}

static amf_t* amf_decode_in_object(const char* buf,int *len)
{
    amf_t      *d,*c;
    uint16_t    slen;
    int         last;
    char        name[64];

    d = (amf_t *)amf_new_object();
    if (d == NULL) {
        return 0;
    }

    while (1) {
        if (*len < 2) {
            goto invalid;
        }

        slen = byte_make_ulong2(buf);
        buf += 2;
        (*len) -= 2;

        if (slen >= 64) {
            goto invalid;
        }

        memcpy(name,buf,slen);
        name[slen] = '\0';

        buf += slen;
        (*len) -= slen;
        last = (*len);

        c = amf_decode_in(buf,len);
        buf += (last - *len);

        if (c == NULL) {
            goto invalid;
        }

        if (c->type == amf_object_end) {
            return d;
        }

        if (amf_put_prop((amf_data_t*)d,name,(amf_data_t*)c) != 0) {
            amf_free_data((amf_data_t*)c);
            goto invalid;
        }
    } 

invalid:
    amf_free_data((amf_data_t*)d);

    return 0;
}

static int amf_encode_in(amf_data_t * data,char* buf,int *len)
{
    amf_t * d;
    uint32_t slen_l,tz;
    int16_t slen;
    int rc,last;
    uint64_t u64;

    d = (amf_t *)data;
    if (!d || (*len) < 3) {
        return -1;
    }

    /*write type*/
    buf[0] = d->type;
    (*len)--;
    buf++;

    switch (d->type) {
    case amf_number:
        if (*len < 8) {
            return -1;
        }
        
        memcpy(&u64,&d->n_val,sizeof(u64));
        ulong_make_byte8(buf,u64);

        buf += 8;
        (*len) -= 8;

        break;

    case amf_boolean:
        if (*len < 1) {
            return -1;
        }

        buf[0] = d->b_val;
        buf += 1;
        (*len) -= 1;

        break;

    case amf_date:
        if (*len < 8 + 2) {
            return -1;
        }

        memcpy(&u64,&d->n_val,sizeof(u64));
        ulong_make_byte8(buf,u64);

        buf += 8;
        (*len) -= 8;

        tz = byte_make_ulong2((char*)d + sizeof(amf_t));
        ulong_make_byte2(buf,tz);

        buf += 2;
        (*len) -= 2;

        break;

    case amf_string:
        slen = d->s_len;

        if ((*len) < (slen + 2)) {
            return -1;
        }

        ulong_make_byte2(buf,slen);
        buf += 2;
        (*len) -= 2;

        memcpy(buf,d->s_val,slen);
        buf += slen;
        (*len) -= slen;

        break;

    case amf_long_string:
        slen_l = d->s_len;

        if ((*len) < (int)(slen_l + 4)) {
            return -1;
        }

        ulong_make_byte4(buf,slen_l);

        buf += 4;
        (*len) -= 4;

        memcpy(buf,d->s_val,slen_l);
        buf += slen_l;
        (*len) -= slen_l;

        break;

    case amf_ecma_array:
        if (*len < 4) {
            return -1;
        }
        
        memset(buf,0,4);
        buf += 4;
        (*len) -= 4;

        /*no break*/

    case amf_object:
        last = *len;

        rc = amf_encode_in_object(d,buf,len);

        if (rc == -1) {
            return -1;
        }

        buf += last - *len;

        break;

    case amf_null:
    case amf_object_end: /*do nothing*/
        break;

    default:
        return -1;
    }

    return (*len);
}

amf_data_t* amf_decode(const char *buf,int *len)
{
    if (!buf ||!len) {
        return 0;
    }

    return (amf_data_t*)amf_decode_in(buf,len);
}

int amf_encode(amf_data_t *data,char *buf,int maxlen)
{
    if (!data || !buf || maxlen <= 0) {
        return -1;
    }

    return amf_encode_in(data,buf,&maxlen);
}

int amf_encode_objects(amf_data_t *objs[],int num, char *buf, int maxlen)
{
    int   i,n,remain;
    char *end,*last;

    if (!objs || num <= 0 || !buf || maxlen <= 0) {
        return -1;
    }

    remain = maxlen;
    last   = buf;
    end    = buf + maxlen;

    for (i = 0;i < num;i++) {
        n = end - last;

        remain = amf_encode_in(objs[i],(char *)last,&n);
        if (remain < 0) {
            return -1;
        }

        last = end - remain;
    }

    return remain;
}

char* amf_get_string(amf_data_t *data)
{
    amf_t *d;

    d = (amf_t*)data;
    if (d->type == amf_long_string || d->type == amf_string) {
        return d->s_val;
    }

    return 0;
}

uint8_t amf_get_bool(amf_data_t *data)
{
    amf_t *d;

    d = (amf_t*)data;
    if (d->type == amf_boolean) {
        return d->b_val;
    }

    return 0;
}

double amf_get_number(amf_data_t *data)
{
    amf_t *d;

    d = (amf_t*)data;
    if (d->type == amf_number) {
        return d->n_val;
    }

    return 0;
}

double  amf_get_date(amf_data_t *data)
{
    amf_t *d;

    d = (amf_t*)data;
    if (d->type == amf_date) {
        return d->n_val;
    }

    return 0;
}

#ifdef HAVE_DEBUG

int amf_dump_data(amf_data_t *data)
{
    const link_t *t,*n;
    const amf_t *d;
    amf_prop_t *prop;
    uint16_t tz;

    if (data == NULL) {
        return 0;
    }

    d = (amf_t*)data;

    switch (d->type) {
    case amf_number:
        printf("double %lf \n",d->n_val);

        break;

    case amf_object:
        printf("object: [ \n");

        t = d->l_val;
        n = t->next;

        while (n != d->l_val) {
            prop = struct_entry(n,amf_prop_t,link);

            printf("[%s] : ",prop->name);
            amf_dump_data(prop->data);

            n = n->next;
        }

        break;

    case amf_ecma_array:
        printf("array: [ \ncount:0\n");

        t = d->l_val;
        n = t->next;

        while (n != d->l_val) {
            prop = struct_entry(n,amf_prop_t,link);

            printf("[%s] : ",prop->name);
            amf_dump_data(prop->data);

            n = n->next;
        }

        break;

    case amf_object_end:
        printf ("]\n");

        break;

    case amf_boolean:
        printf("boolean %d\n",d->b_val);

        break;

    case amf_date:
        tz = byte_make_ulong2((char*)d + sizeof(amf_t));
        printf("date %lf,%d",d->n_val,tz);
        
        break;

    case amf_long_string:
    case amf_string:
        printf("string \"%s\"\n",d->s_val);

        break;

    case amf_null:
        printf("null\n");
        break;

    case amf_undefined:
        printf("undefined\n");
        break;

    default:
        printf("be not supported!\n");
        break;
    }

    return 0;
}

#endif
