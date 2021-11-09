#include "io_helper.h"
#include "request.h"
#include <limits.h>

#define MAXBUF (8192)


//
//    TODO: add code to create and manage the buffer
//
int init=0; // GLOBAL VARIABLE TO MAKE SURE IF request_handle() function is called for the first time
//pthread_mutex_t init_lock= PTHREAD_MUTEX_INITIALIZER; //lock for intialization of buffer
pthread_mutex_t m= PTHREAD_MUTEX_INITIALIZER;; //lock
pthread_cond_t fill= PTHREAD_COND_INITIALIZER; //CV
pthread_cond_t empty=PTHREAD_COND_INITIALIZER; //CV


typedef struct request_struct  //structure to hold the request information to be saved in buffer
{
    int fd;
    char filename[MAXBUF];
    int filesize;

}request;


typedef struct bufferQueue //buffer structure - circular queue
{
    int size;                   //size of the buffer i.e. user input
    request buffer[MAXBUF];     //the array fro holding requests.
    int in;                     
    int out;
    int numfull;                //current valid requests present in the array

}buffer_queue;

buffer_queue _buff_queue; //declaring global variable buffer_queue

request create_request(int fd, char *filename, int filesize)
{
    request req;
    req.fd=fd;
    strcpy(req.filename,filename);
    req.filesize=filesize;
    return req;
}

void initialize_buffer()                   //initialization func for buffer
{
    //printf("init buffer ... buffer size is %d\n",buffer_max_size);
    _buff_queue.size=buffer_max_size;    //set size of buffer
    _buff_queue.in=0;
    _buff_queue.out=0;
    _buff_queue.numfull=0;

   // _buff_queue.buffer=malloc(_buff_queue.size* sizeof(request));

}

void insert_into_buffer(int fd, char *filename, int filesize)
{
    //request req=create_request(fd,filename,filesize);
    _buff_queue.buffer[_buff_queue.in].fd=fd;                       
    strcpy(_buff_queue.buffer[_buff_queue.in].filename,filename);
    _buff_queue.buffer[_buff_queue.in].filesize=filesize;
    _buff_queue.in=(_buff_queue.in+1)%(_buff_queue.size); //change in to point at next empty location in buffer
    _buff_queue.numfull++;                                 //increment numfull
}
request remove_from_buffer_FIFO()            //REMOVAL FROM QUEUE IN FIFO MANNER
{
    request req=_buff_queue.buffer[_buff_queue.out];
    _buff_queue.buffer[_buff_queue.out].filesize=-1; //removing the buffer.. better for checking in SFF
    _buff_queue.out=(_buff_queue.out +1)%(_buff_queue.size);    //change out to point at next valid location in buffer
    _buff_queue.numfull--;                              //decrement numfull
    return req;
}

request remove_from_buffer_SFF()               //REMOVAL FROM BUFFER IN SFF MANNER
{
    int i;
    int min = INT_MAX, min_index;
   //debugging  printf("min before loop is %d\n",min);
   
   //when in>out find the request having min filesize and min_index will be in the range [out,in)
    if(_buff_queue.out < _buff_queue.in)
    {
        for(i = _buff_queue.out; i < _buff_queue.in; i++)
        {
            if(_buff_queue.buffer[i].filesize >0 && _buff_queue.buffer[i].filesize < min)
            {
                min = _buff_queue.buffer[i].filesize;
                min_index = i;
            }
        }
    }
    //when in<out we run the for loop from out to size and from 0 to in
    else
    {
        //this loop checks for  min filesize from index out to _buff_queue.size-1
        for(i = _buff_queue.out; i < _buff_queue.size; i++)
        {    //printf("else 1. file size %d\n",_buff_queue.buffer[i].filesize);
            if(_buff_queue.buffer[i].filesize < min)
            {
                min = _buff_queue.buffer[i].filesize;
                min_index = i;
            }
        }
        //this loop checks for min filesize from index 0 to in-1
        for(i = 0; i < _buff_queue.in; i++)
        { //printf("else 2. file size %d\n",_buff_queue.buffer[i].filesize);
            if(_buff_queue.buffer[i].filesize >0 &&  _buff_queue.buffer[i].filesize < min)
            {
                min = _buff_queue.buffer[i].filesize;
                min_index = i;
            }
        }
    }
    //we now get request with min filesize at min_index index  
    //NOW SWAP THIS REQUEST WITH REQUEST AT INDEX OUT SO THAT IT CAN BE REMOVED IN NORMAL QUEUE REMOVAL WAY
    request req=_buff_queue.buffer[min_index];
    _buff_queue.buffer[min_index]=_buff_queue.buffer[_buff_queue.out]; //placed request at index out in place of request at index min_index , now we can remove out
    _buff_queue.buffer[_buff_queue.out].filesize=-1;           
    _buff_queue.out=(_buff_queue.out +1)%(_buff_queue.size);
    _buff_queue.numfull--;
    return req;

}
//
// Sends out HTTP response in case of errors
//
void request_error(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg) {
    char buf[MAXBUF], body[MAXBUF];

    // Create the body of error message first (have to know its length for header)
    sprintf(body, ""
        "<!doctype html>\r\n"
        "<head>\r\n"
        "  <title>OSTEP WebServer Error</title>\r\n"
        "</head>\r\n"
        "<body>\r\n"
        "  <h2>%s: %s</h2>\r\n"
        "  <p>%s: %s</p>\r\n"
        "</body>\r\n"
        "</html>\r\n", errnum, shortmsg, longmsg, cause);

    // Write out the header information for this response
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    write_or_die(fd, buf, strlen(buf));

    sprintf(buf, "Content-Type: text/html\r\n");
    write_or_die(fd, buf, strlen(buf));

    sprintf(buf, "Content-Length: %lu\r\n\r\n", strlen(body));
    write_or_die(fd, buf, strlen(buf));

    // Write out the body last
    write_or_die(fd, body, strlen(body));

    // close the socket connection
    close_or_die(fd);
}

//
// Reads and discards everything up to an empty text line
//
void request_read_headers(int fd) {
    char buf[MAXBUF];

    readline_or_die(fd, buf, MAXBUF);
    while (strcmp(buf, "\r\n")) {
        readline_or_die(fd, buf, MAXBUF);
    }
    return;
}

//
// Return 1 if static, 0 if dynamic content (executable file)
// Calculates filename (and cgiargs, for dynamic) from uri
//
int request_parse_uri(char *uri, char *filename, char *cgiargs) {
    char *ptr;

    if (!strstr(uri, "cgi")) {
    // static
    strcpy(cgiargs, "");
    sprintf(filename, ".%s", uri);
    if (uri[strlen(uri)-1] == '/') {
        strcat(filename, "index.html");
    }
    return 1;
    } else {
    // dynamic
    ptr = index(uri, '?');
    if (ptr) {
        strcpy(cgiargs, ptr+1);
        *ptr = '\0';
    } else {
        strcpy(cgiargs, "");
    }
    sprintf(filename, ".%s", uri);
    return 0;
    }
}

//
// Fills in the filetype given the filename
//
void request_get_filetype(char *filename, char *filetype) {
    if (strstr(filename, ".html"))
        strcpy(filetype, "text/html");
    else if (strstr(filename, ".gif"))
        strcpy(filetype, "image/gif");
    else if (strstr(filename, ".jpg"))
        strcpy(filetype, "image/jpeg");
    else
        strcpy(filetype, "text/plain");
}

//
// Handles requests for static content
//
void request_serve_static(int fd, char *filename, int filesize) {
    int srcfd;
    char *srcp, filetype[MAXBUF], buf[MAXBUF];

    request_get_filetype(filename, filetype);
    srcfd = open_or_die(filename, O_RDONLY, 0);

    // Rather than call read() to read the file into memory,
    // which would require that we allocate a buffer, we memory-map the file
    srcp = mmap_or_die(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
    close_or_die(srcfd);

    // put together response
    sprintf(buf, ""
        "HTTP/1.0 200 OK\r\n"
        "Server: OSTEP WebServer\r\n"
        "Content-Length: %d\r\n"
        "Content-Type: %s\r\n\r\n",
        filesize, filetype);

    write_or_die(fd, buf, strlen(buf));

    //  Writes out to the client socket the memory-mapped file
    write_or_die(fd, srcp, filesize);
    munmap_or_die(srcp, filesize);
}

//
// Fetches the requests from the buffer and handles them (thread locic)
//
void* thread_request_serve_static(void* arg)
{
    // TODO: write code to actualy respond to HTTP requests
    while(1)
    {
        pthread_mutex_lock(&m);
        if(init==0) //initializing buffer when the function is invoked for the first time
        {
            initialize_buffer();
            init++;
        }

        while(_buff_queue.numfull==0)           //if buffer is empty, WAIT! CV used is fill
        {
            pthread_cond_wait(&fill,&m);
        }
        request req;
        //REMOVAL ACCORDING TO SCHEDULING ALGORITHM MENTIONED BY THE USER
        if(scheduling_algo==0)
            req=remove_from_buffer_FIFO();
        else
            req=remove_from_buffer_SFF();

       printf("Request for %s is removed from the buffer - SIZE %d.\n",req.filename,req.filesize);

        pthread_cond_signal(&empty);        //signal for server that it can now write because there is an empty place in buffer. CV used is empty
        pthread_mutex_unlock(&m);           //release the lock as CS is over

        request_serve_static(req.fd,req.filename,req.filesize);     // every thread calls this function which will then serve the request

        close_or_die(req.fd);                               //CLOSE THE fd

    }
}

//
// Initial handling of the request
//
void request_handle(int fd) {
    int is_static;
    struct stat sbuf;
    char buf[MAXBUF], method[MAXBUF], uri[MAXBUF], version[MAXBUF];
    char filename[MAXBUF], cgiargs[MAXBUF];

    // get the request type, file path and HTTP version
    readline_or_die(fd, buf, MAXBUF);
    sscanf(buf, "%s %s %s", method, uri, version);
    printf("method:%s uri:%s version:%s\n", method, uri, version);

    // verify if the request type is GET is not
    if (strcasecmp(method, "GET")) {
        request_error(fd, method, "501", "Not Implemented", "server does not implement this method");
        return;
    }
    request_read_headers(fd);

    // check requested content type (static/dynamic)
    is_static = request_parse_uri(uri, filename, cgiargs);

    // get some data regarding the requested file, also check if requested file is present on server
    if (stat(filename, &sbuf) < 0) {
        request_error(fd, filename, "404", "Not found", "server could not find this file");
        return;
    }

    // verify if requested content is static
    if (is_static) {
        if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) {
            request_error(fd, filename, "403", "Forbidden", "server could not read this file");
            return;
        }

        // TODO: write code to add HTTP requests in the buffer based on the scheduling policy
    //SECURITY 
    //CHECKING IF PATH NAME CONSISTS OF ".."
    char s[]="..";
    char * p = strstr(filename,s); //Returns null if string ".." not found in filename
    if (p!=NULL)
    {
        request_error(fd,filename,"403","Forbidden","Traversing up in filesystem is not allowed");
        return;
    }
    
    int filesize=sbuf.st_size ;//GET FILESIZE

    pthread_mutex_lock(&m); //LOCK IS HELD
    if(init==0) //initializing buffer when the function is invoked for the first time
    {
        initialize_buffer();
        init++;     //increment value of init variable so this if condition fails next time
    }
    
    while(_buff_queue.numfull==_buff_queue.size) //if buffer is FULL, WAIT! CV used is empty
    {
        pthread_cond_wait(&empty,&m);
    }
    insert_into_buffer(fd,filename,filesize); //insert current trequest

    printf("Request for %s is added to the buffer SIZE %d.\n",filename,filesize);
    pthread_cond_signal(&fill);             //signal for client that the buffer is not empty so they can read. CV used is fill
    pthread_mutex_unlock(&m); //RELEASE THE LOCK as CS is over

    } else {
        request_error(fd, filename, "501", "Not Implemented", "server does not serve dynamic content request");
    }
}








