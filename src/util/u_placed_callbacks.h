#ifndef _U_PLACED_CALLBACKS_H
#define _U_PLACED_CALLBACKS_H

#ifdef __cplusplus
extern "C" {
#endif

/* modelled after u_debug */
struct util_placed_callbacks
{
   /**
    * Callback for the driver get a placed allocation from the frontend
    *
    * \param data   user-supplied context pointer (holds GL_placed_allocator callbacks), not the be confused for mapping data
    * \param size   desired size of the placed allocation
    * \param token  pointer to a token that the driver must pass to placed_unmap (holds unmap callback, which may have been switched out)
    * 
    * \return placed mapping address
    */
   void* (*placed_map)(unsigned int size,
                       void **token);

   /**
    * Callback for the driver get a placed allocation from the frontend
    *
    * \param token mapping token recieved from placed_map in order to call correct unmap
    * 
    * \return placed mapping address
    */
   void (*placed_unmap)(void *mapping, unsigned int size, void *token);
};

static inline void *u_placed_map(struct util_placed_callbacks *cbs, unsigned int size, void **token)
{
   if (cbs->placed_map) return cbs->placed_map(size, token);
   return NULL;
}

static inline void u_placed_unmap(struct util_placed_callbacks *cbs, void *mapping, unsigned int size, void *token)
{
   if (cbs->placed_unmap) cbs->placed_unmap(mapping, size, token);
}



#ifdef __cplusplus
} // extern "C" {
#endif

#endif /* _U_PLACED_CALLBACKS_H */
