// Scaffolding originally generated from include/server.x.
// Edit to add functionality.

#include <cstdint>
#include <cassert>

#include "server/chubby_server.h"

#include "server/serverimpl.hh"

const uint64_t READ = 0x1;
const uint64_t WRITE = 0x2;
const uint64_t CREATE_DIRECTORY = 0x4;
const uint64_t CREATE_FILE = 0x8;

enum ClientError {
  BAD_ARG,
  FS_FAIL
};

using std::cout;
using std::endl;

void
api_v1_server::printFd()
{
  cout << "\tfile2fd_map:" << endl;
  for (auto it = file2fd_map.begin(); 
       it != file2fd_map.end(); it++) {
    cout << "\t\t file: "<< it->first<<endl;
    for (auto pair : it->second)
      cout<< "\t\t\t client: "<<pair.client
	  << ", FD: ("<< (pair.fd)->file_name
	  << ", "<< (pair.fd)->instance_number<<")"<<endl;
  }

  cout << "\tclient2fd_map:" <<endl;
  for (auto it = client2fd_map.begin(); 
       it != client2fd_map.end(); it++) {
    cout << "\t\t client: "<< it->first<<endl;
    for (auto fd : it->second)
      cout << "\t\t\t FD: ("<< fd ->file_name
	   << ", "<< fd->instance_number<<")"<<endl;
  }

  cout << "\tlock_queue_map:" <<endl;
  for (auto it = lock_queue_map.begin();
       it != lock_queue_map.end(); it++) {
    cout << "\t\t file: "<< it->first<<endl;
    for (auto& rpc : it->second)
      cout << "\t\t\t Session: ("<< rpc.session
	   << ", "<< rpc.xid <<")"<<endl;
  }
}

std::unique_ptr<int>
api_v1_server::increment(std::unique_ptr<int> arg,
                         xdr::SessionId session_id, uint32_t xid)
{
  std::unique_ptr<int> res(new int);
  *res = *arg + 1;
  EventContent evc;
  evc.event = ChubbyEvent::NOP;
  evc.fname = std::string("inc");
  chubby_server_->send<event_interface::event_callback_t>(session_id, evc);
  chubby_server_->reply(session_id, xid, std::move(res));
  evc.fname = std::string("inc done");
  chubby_server_->send<event_interface::event_callback_t>(session_id, evc);
  return res;
}

std::unique_ptr<int>
api_v1_server::decrement(std::unique_ptr<int> arg,
                         xdr::SessionId session_id, uint32_t xid)
{
  std::unique_ptr<int> res(new int);
  *res = *arg - 1;
  EventContent evc;
  evc.event = ChubbyEvent::NOP;
  evc.fname = std::string("dec");
  chubby_server_->send<event_interface::event_callback_t>(session_id, evc);
  chubby_server_->reply(session_id, xid, std::move(res));
  evc.fname = std::string("dec done");
  chubby_server_->send<event_interface::event_callback_t>(session_id, evc);
  return res;
}

std::unique_ptr<RetFd>
api_v1_server::fileOpen(std::unique_ptr<ArgOpen> arg,
                        xdr::SessionId session_id, uint32_t xid)
{
  std::unique_ptr<RetFd> res(new RetFd);
  std::string file_name = arg->name;
  Mode mode = arg->mode;
  std::string client_id = chubby_server_->getClientId(session_id);

  cout<<"\nserver: fileOpen: ("<< file_name << ", "<< mode <<")"<<endl;

  if(!checkName(file_name)) {
    // return with error
    res->discriminant(1);
    res->errCode() = BAD_ARG;
    chubby_server_->reply(session_id, xid, std::move(res));
    return res;
  }   
  
  if((mode & CREATE_DIRECTORY) && (mode & CREATE_FILE)) {
    // return with error
    res->discriminant(1);
    res->errCode() = BAD_ARG;
    chubby_server_->reply(session_id, xid, std::move(res));
    return res;
  }

  /* 
  // check if client has already opened this file
  auto l = client2fd_map[client_id];
  for (auto it = l.begin(); it != l.end(); ++it)
    if ((*it)->file_name.compare(file_name) == 0) {
      // FD with the same file_name exists
      // TODO return error
    }
  */
  
  FileHandler *fd = new FileHandler();
  // Create file or dir
  if((mode & CREATE_DIRECTORY) || (mode & CREATE_FILE)) {
    bool is_dir = mode & CREATE_DIRECTORY;
    ++(this->instance_number);

    if(!db.checkAndCreate(file_name, is_dir, this->instance_number)) {
      // creation failed, then return false
      res->discriminant(1);
      res->errCode() = FS_FAIL;
      chubby_server_->reply(session_id, xid, std::move(res));
      return res;
    }
    fd->instance_number = this->instance_number;
  } else { // open an existing file or dir
    // check file is exist
    uint64_t instance_number;
    if(!db.checkAndOpen(file_name, &instance_number)) {
      // open failed, then return false
      res->discriminant(1);
      res->errCode() = FS_FAIL;
      chubby_server_->reply(session_id, xid, std::move(res));
      return res;      
    }
    fd->instance_number = instance_number;
  }
  fd->magic_number = this->rand_gen();
  fd->master_sequence_number = this->master_sequence_number;
  fd->file_name = file_name;
  fd->write_is_allowed = mode & WRITE;

  // add FD to <file, list of (client, FD) pairs> map
  //std::map<std::string, std::list<ClientFdPair> > file2fd_map;
  ClientFdPair pair;
  pair.client = client_id;
  pair.fd = fd;
  file2fd_map[file_name].push_back(pair);

  // add FD to <client, list of FDs> map
  // std::map<uint64_t, std::list<FileHandler* > > client2fd_map;
  client2fd_map[client_id].push_back(fd);

  // return normally with FD
  res->discriminant(0);
  res->val() = *fd;
  printFd();
  chubby_server_->reply(session_id, xid, std::move(res));
  return res;
}

std::unique_ptr<RetBool>
api_v1_server::fileClose(std::unique_ptr<FileHandler> arg,
			 xdr::SessionId session_id, uint32_t xid)
{
  std::unique_ptr<RetBool> res(new RetBool);
  std::string client_id = chubby_server_->getClientId(session_id);
  
  cout<<"\nserver: fileClose: ("<< arg->file_name << ", "<< arg->instance_number<<")"<<endl;

  FileHandler *fd = findFd(client_id, *arg);
  if(fd == nullptr) {
    // No match FD found
    // return normally with TRUE value
    res->discriminant(1);
    res->errCode() = BAD_ARG;
    chubby_server_->reply(session_id, xid, std::move(res));
    return res;
  }

  ClientFdPair pair {client_id, fd};
  // remove FD from <file, list of (client, FD) pairs> map
  file2fd_map[fd->file_name].remove(pair);
  if(file2fd_map[fd->file_name].empty())
    file2fd_map.erase(fd->file_name);

  // remove FD from <client, list of FDs> map
  client2fd_map[client_id].remove(fd);
  if(client2fd_map[client_id].empty())
    client2fd_map.erase(client_id);

  // delete FD
  delete fd;
  
  // return normally with TRUE value
  res->discriminant(0);
  res->val() = true;
  printFd();
  chubby_server_->reply(session_id, xid, std::move(res));
  return res;
}

std::unique_ptr<RetBool>
api_v1_server::fileDelete(std::unique_ptr<FileHandler> arg,
                          xdr::SessionId session_id, uint32_t xid)
{
  std::unique_ptr<RetBool> res(new RetBool);
  std::string client_id = chubby_server_->getClientId(session_id);
  
  cout<<"\nserver: fileDelete: ("<< arg->file_name
      << ", "<< arg->instance_number<<")"<<endl;
  
  FileHandler *fd = findFd(client_id, *arg);
  if(fd == nullptr) {
    // No match FD found
    // return an error
    res->discriminant(1);
    res->errCode() = BAD_ARG;
    chubby_server_->reply(session_id, xid, std::move(res));
    return res;
  }
  
  // try to delete in the database
  if(!db.checkAndDelete(fd->file_name, fd->instance_number)) {
    // Delete failed, return false
    res->discriminant(0);
    res->val() = false;
    chubby_server_->reply(session_id, xid, std::move(res));
    return res;
  }
  
  // remove all FDs in file2fd_map[fd->file_name]
  std::list<ClientFdPair> l = file2fd_map[arg->file_name];
  for (auto it = l.begin(); it != l.end(); ++it) {
    std::string c = it->client;
    FileHandler *f = it->fd;
    // remove this FD in client2fd_map
    client2fd_map[c].remove(f);
    if(client2fd_map[c].empty())
      client2fd_map.erase(c);
    // free space
    delete f;
  }  
  // remove the list in file2fd_map
  int r = file2fd_map.erase(arg->file_name);
  assert(r == 1);
  
  // return normally with TRUE value
  res->discriminant(0);
  res->val() = true;
  printFd();
  chubby_server_->reply(session_id, xid, std::move(res));
  return res;
}

std::unique_ptr<RetContentsAndStat>
api_v1_server::getContentsAndStat(std::unique_ptr<FileHandler> arg,
                                  xdr::SessionId session_id, uint32_t xid)
{
  std::unique_ptr<RetContentsAndStat> res(new RetContentsAndStat);
  std::string client_id = chubby_server_->getClientId(session_id);
  
  FileHandler *fd = findFd(client_id, *arg);
  if(fd == nullptr) {
    // No match FD found
    res->discriminant(1);
    res->errCode() = BAD_ARG;
    chubby_server_->reply(session_id, xid, std::move(res));
    return res;
  }

  std::string content;
  MetaData meta;
  // try to read in the database
  if(!db.checkAndRead(fd->file_name, fd->instance_number, &content, &meta)) {
    // Read failed
    res->discriminant(1);
    res->errCode() = FS_FAIL;
    chubby_server_->reply(session_id, xid, std::move(res));
    return res;
  }

  // return normally with ContentsAndStat
  res->discriminant(0);
  res->val().content = content;
  res->val().stat = meta;
  chubby_server_->reply(session_id, xid, std::move(res));
  return res;
}

std::unique_ptr<RetBool>
api_v1_server::setContents(std::unique_ptr<ArgSetContents> arg,
                           xdr::SessionId session_id, uint32_t xid)
{
  std::unique_ptr<RetBool> res(new RetBool);
  std::string client_id = chubby_server_->getClientId(session_id);
  
  FileHandler *fd = findFd(client_id, arg->fd);
  if(fd == nullptr) {
    // No match FD found
    res->discriminant(1);
    res->errCode() = BAD_ARG;
    chubby_server_->reply(session_id, xid, std::move(res));
    return res;
  }

  // try to update in the database
  if(!db.checkAndUpdate(fd->file_name, fd->instance_number, arg->content)) {
    // Update failed
    res->discriminant(1);
    res->errCode() = FS_FAIL;
    chubby_server_->reply(session_id, xid, std::move(res));
    return res;
  }
  
  // return normally with TRUE value
  res->discriminant(0);
  res->val() = true;
  chubby_server_->reply(session_id, xid, std::move(res));
  return res;
}

std::unique_ptr<RetBool>
api_v1_server::acquire(std::unique_ptr<FileHandler> arg,
                       xdr::SessionId session_id, uint32_t xid)
{
  std::unique_ptr<RetBool> res(new RetBool);
  std::string client_id = chubby_server_->getClientId(session_id);
  
  cout<<"\nserver: acquire: ("<< arg->file_name << ", "
      << arg->instance_number<<", "<<client_id<<")"<<endl;
  
  FileHandler *fd = findFd(client_id, *arg);
  if(fd == nullptr) {
    // No match FD found
    // return an error
    res->discriminant(1);
    res->errCode() = BAD_ARG;
    chubby_server_->reply(session_id, xid, std::move(res));
    return res;
  }

  // try to set the lock_owner in the database
  if(db.testAndSetLockOwner(fd->file_name, fd->instance_number, client_id)) {
    // succeeded in DB, return true
    assert(lock_queue_map.find(fd->file_name) == lock_queue_map.end());
    res->discriminant(0);
    res->val() = true;
    chubby_server_->reply(session_id, xid, std::move(res));
    printFd();
    return res;
  }

  // otherwise, add session information in lock_queue_map
  lock_queue_map[fd->file_name].push_back({session_id, xid});
  printFd();
  return res;
}

std::unique_ptr<RetBool>
api_v1_server::tryAcquire(std::unique_ptr<FileHandler> arg,
                          xdr::SessionId session_id, uint32_t xid)
{
  std::unique_ptr<RetBool> res(new RetBool);
  std::string client_id = chubby_server_->getClientId(session_id);
  
  cout<<"\nserver: tryAcquire: ("<< arg->file_name << ", "
      << arg->instance_number<<", "<<client_id<<")"<<endl;

  FileHandler *fd = findFd(client_id, *arg);
  if(fd == nullptr) {
    // No match FD found
    // return an error
    res->discriminant(1);
    res->errCode() = BAD_ARG;
    chubby_server_->reply(session_id, xid, std::move(res));
    return res;
  }

  // try to set the lock_owner in the database
  if(db.testAndSetLockOwner(fd->file_name, fd->instance_number, client_id)) {
    // succeeded in DB, return true
    assert(lock_queue_map.find(fd->file_name) == lock_queue_map.end());
    res->discriminant(0);
    res->val() = true;
    chubby_server_->reply(session_id, xid, std::move(res));
    printFd();
    return res;
  }

  // return false otherwise
  res->discriminant(0);
  res->val() = false;
  chubby_server_->reply(session_id, xid, std::move(res));
  printFd();
  return res;
}

std::unique_ptr<RetBool>
api_v1_server::release(std::unique_ptr<FileHandler> arg,
                       xdr::SessionId session_id, uint32_t xid)
{
  std::unique_ptr<RetBool> res(new RetBool);
  std::string client_id = chubby_server_->getClientId(session_id);

  cout<<"\nserver: release: ("<< arg->file_name << ", "
      << arg->instance_number<<", "<<client_id<<")"<<endl;

  FileHandler *fd = findFd(client_id, *arg);
  if(fd == nullptr) {
    // No match FD found
    // return an error
    res->discriminant(1);
    res->errCode() = BAD_ARG;
    chubby_server_->reply(session_id, xid, std::move(res));
    return res;
  }

  if(!db.resetLockOwner(fd->file_name, fd->instance_number)) {
    // reset failed
    res->discriminant(1);
    res->errCode() = FS_FAIL;
    chubby_server_->reply(session_id, xid, std::move(res));
    return res;
  }

  // reset successed
  res->discriminant(0);
  res->val() = true;
  chubby_server_->reply(session_id, xid, std::move(res));
  
  printFd();
  cout << "check the lock_queue_map"<<endl;
  // check the lock_queue_map
  auto it = lock_queue_map.find(fd->file_name);
  if(it != lock_queue_map.end()) {
    std::list<RPCIdPair> &lock_queue = it->second;
    assert(lock_queue.size() != 0);
    
    // pop the first element in the queue
    RPCIdPair acquire_rpc = lock_queue.front();
    lock_queue.pop_front();
    
    // reply acquire() PRC
    std::unique_ptr<RetBool> r(new RetBool);
    r->discriminant(0);
    r->val() = true;
    chubby_server_->reply(acquire_rpc.session, acquire_rpc.xid, std::move(r));
        
    // garbage collect
    if(lock_queue.empty())
      lock_queue_map.erase(it);
    printFd();
  }
  return res;
}


/* Returns true is C is any letter, numbers, underscore, or slash. */
inline bool
checkChar(char c)
{
  std::string validChars = "_/0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
  return validChars.find(c) != std::string::npos;
}

/* Paths must begin with a '/' and must contain only letters, numbers, 
   underscores, and single slashes to separate components. 
   Cannot be "/". Cannot end with '/'.*/
bool 
api_v1_server::checkName(const std::string &key)
{
  // key must begin with '/'
  if (key[0] != '/') return false;
  // path cannot end "/"
  if (key[key.length() - 1] == '/') return false;
  // no space
  for (int i = 0; i < key.length(); ++i)
    {
      // paths must contain only letters, numbers, underscores, and slashes to separate components
      if(!checkChar(key[i])) return false;
    }
  // path cannot contain "//"
  if(key.find("//") != std::string::npos) return false;
  return true;
}


FileHandler *
api_v1_server::findFd(std::string client_id, const FileHandler &fd)
{
  std::list<FileHandler *> l = client2fd_map[client_id];
    
  for(auto it = l.begin(); it != l.end(); ++it) {
    FileHandler *p = *it;
    /*
    cout << "findFd: "<< p->instance_number
	 << p->magic_number << p->master_sequence_number
	 << p->file_name << p->write_is_allowed<<endl;
    */
    if (p->instance_number == fd.instance_number &&
	p->magic_number == fd.magic_number &&
	p->master_sequence_number == fd.master_sequence_number &&
	p->file_name.compare(fd.file_name) == 0 &&
	p->write_is_allowed == fd.write_is_allowed)
      return p;
  }
  // return nullptr if no match found
  return nullptr;
}
