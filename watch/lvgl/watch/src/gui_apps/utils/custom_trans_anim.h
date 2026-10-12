/**
 * @file lv_area.h
 *
 */

#ifndef CUSTOM_TRANS_ANIM_H
#define CUSTOM_TRANS_ANIM_H

#ifdef __cplusplus
extern "C" {
#endif


typedef enum
{
    CUST_ANIM_TYPE_0,
    CUST_ANIM_TYPE_1,
    CUST_ANIM_TYPE_2,
    CUST_ANIM_TYPE_3,
    CUST_ANIM_TYPE_PUSH,    /* 水平平移：进入向左推、返回向右退（取代 0..3 的演示轮换） */
    CUST_ANIM_TYPE_MAX,
} CUST_ANIM_TYPE_E;

void cust_trans_anim_config(CUST_ANIM_TYPE_E type, lv_point_t *pivot);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif

