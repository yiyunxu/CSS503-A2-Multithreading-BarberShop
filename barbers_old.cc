//
// barbers.cc
//
// Simulate the sleeping barbers problem.
//
// Author: Morris Bernstein
// Copyright 2019, Systems Deployment, LLC.


#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <pthread.h>
#include <queue>
#include <random>
#include <time.h>
#include <unistd.h>
#include <vector>

using namespace std;

const char* PROG_NAME = "";


class Shop;
class Barber;
class Customer;


class Lock {
public:
  Lock(pthread_mutex_t* mutex): mutex(mutex) {
    int rc = pthread_mutex_lock(this->mutex);
    if (rc < 0) {
      perror("can't lock mutex");
      exit(EXIT_FAILURE);
    }
  }

  ~Lock() {
    int rc = pthread_mutex_unlock(mutex);
    if (rc < 0) {
      perror("can't unlock mutex");
      exit(EXIT_FAILURE);
    }
  }

private:
  pthread_mutex_t* mutex;
};


class Shop {
public:
  struct BarberOrWait {
    Barber* barber;		// Barber is available
    bool chair_available;	// If barber isn't available,
				// waiting chair is available.
  };

  // Constructor initializes shop and creates Barber threads (which
  // will immediately start calling next_customer to fill the
  // collection of sleeping barbers).
  Shop(int n_barbers,
       unsigned waiting_chairs,
       int average_service_time,
       int service_time_deviation,
       int average_customer_arrival,
       int duration);

  // Main thread: open the shop and spawn customer threads until
  // closing time.  Report summary statistics for the day.
  void run();

  // Customer thread announces arrival to shop. If the collection of
  // currently sleeping barbers is not empty, remove and return one
  // barber from the collection. If all the barbers are busy and there
  // is an empty chair in the waiting room, add the customer to the
  // waiting queue and return {nullptr, true}.  Otherwise, the
  // customer will leave: return {nullptr, false}.
  BarberOrWait arrives(Customer* customer);

  // Barber thread requests next customer.  If no customers are
  // currently waiting, add the barber to the collection of
  // currently sleeping barbers and return nullptr.
  Customer* next_customer(Barber* barber);

  // Return random service time.
  int service_time();

  // Return random customer arrival.
  int customer_arrival_time();

  static int customers_served_immediately;
  static int customers_waited;
  static int customers_turned_away;
  static int customers_total;
  
  static queue<Barber*> sleepingBarbers;
  static queue<Customer*> waitingCustomers;
  
private:
  vector<pthread_t*> barber_threads;
  struct timespec time_limit;

  int n_barbers;
  unsigned waiting_chairs;
  int average_service_time;
  int service_time_deviation;
  int average_customer_arrival;
  int duration;

  vector<Barber*> barbers;
  pthread_mutex_t *mutex;     // Protect shared access.
};


class Barber {
public:
  Barber(Shop* shop, int id);

  // Barber thread function.
  void run();

  // Shop tells barber it's closing time.
  void closing_time();

  // Customer wakes up barber.
  void awaken(Customer* customer);

  // Customer sits in barber's chair.
  void customer_sits();

  // Customer proffers payment.
  void payment();

  const int id;

  enum class barber_state {SLEEP, AWAKE, SERVICE, ACCEPT};
  
private:
  Shop* shop;
  Customer *customer;

  bool closing; // whether shop is closing
  barber_state state;  // one of enum barber_state

  pthread_mutex_t *b_mutex;     // Protect shared access
  pthread_cond_t *b_condition; // Condition variable
};


class Customer {
public:
  Customer(Shop* shop, int id);
  ~Customer();

  // Customer thread function.
  void run();

  // Barber calls this customer after checking the waiting-room
  // queue (customer should be waiting).
  void next_customer(Barber* barber);

  // Barber finishes haircut (Customer should be receiving haircut).
  void finished();

  // Barber has accepted payment for service.
  void payment_accepted();

  const int id;

  enum class customer_state {WAIT, GOTOBARBER, RECEIVESERVICE, PAYMENT, LEAVE};
  
  void setBarber(Barber *barber);
  customer_state state; // one of enum customer_state

  pthread_mutex_t *c_mutex;     // Protect shared access
  pthread_cond_t *c_condition; //  Condition variable
private:
  Shop* shop;
  Barber* barber;

  
  
};

void Customer::setBarber(Barber *barber){
  this->barber = barber;
}

int Shop::customers_served_immediately;
int Shop::customers_waited;
int Shop::customers_turned_away;
int Shop::customers_total;
  
queue<Barber*> Shop::sleepingBarbers;
queue<Customer*> Shop::waitingCustomers;

// Barber thread.  Must be joined by Shop thread to allow shop to
// determine when barbers have left.
void* run_barber(void* arg) {
  Barber* barber = reinterpret_cast<Barber*>(arg);
  barber->run();
  return nullptr;
}


// Customer thread.  Runs in detatched mode so resources are
// automagically cleaned up when customer leaves shop.
void* run_customer(void* arg) {
  Customer* customer = reinterpret_cast<Customer*>(arg);
  customer->run();
  delete customer;
  return nullptr;
}


// Constructor initializes shop and creates Barber threads (which
// will immediately start calling next_customer to fill the
// collection of sleeping barbers).
Shop::Shop(int n_barbers,
	   unsigned int waiting_chairs,
	   int average_service_time,
	   int service_time_deviation,
	   int average_customer_arrival,
	   int duration)
{
  this->n_barbers = n_barbers;
  this->waiting_chairs = waiting_chairs;
  this->average_service_time = average_service_time;
  this->service_time_deviation = service_time_deviation;
  this->average_customer_arrival = average_customer_arrival;
  this->duration = duration;

  customers_served_immediately= 0;
  customers_waited = 0;
  customers_turned_away = 0;
  customers_total = 0;
  
  mutex = nullptr;
  mutex = reinterpret_cast<pthread_mutex_t*>(malloc(sizeof(pthread_mutex_t)));
  pthread_mutex_init(mutex, NULL);

  int rc = clock_gettime(CLOCK_REALTIME, &time_limit);
  if (rc < 0) {
    perror("reading realtime clock");
    exit(EXIT_FAILURE);
  }
  // Round to nearest second
  time_limit.tv_sec += duration + (time_limit.tv_nsec >= 500000000);
  time_limit.tv_nsec = 0;

  // Initializes barbers
  for (int i = 0; i < n_barbers; i++) {
    barbers.push_back(new Barber(this, i));
  }

  // creates Barber threads
  for (auto barber: Shop::barbers) {
    pthread_t* thread = reinterpret_cast<pthread_t*>(calloc(1, sizeof(pthread_t)));
    int rc = pthread_create(thread, nullptr,
                              ::run_barber, reinterpret_cast<void *>(barber));
    cout<<"barber "<<barber->id <<" arrives for work"<<endl;
    if (rc != 0) {
        errno = rc;
        perror("creating pthread");
        exit(EXIT_FAILURE);
      }
    barber_threads.push_back(thread);
  }
}


// Main thread: open the shop and spawn customer threads until
// closing time.  Report summary statistics for the day.
void Shop::run() {
  cout << "the shop opens" << endl;

  for (int next_customer_id = 0; ; next_customer_id++) {
    struct timespec now;
    int rc = clock_gettime(CLOCK_REALTIME, &now);
    if (rc < 0) {
      perror("reading realtime clock");
      exit(EXIT_FAILURE);
    }
    if (now.tv_sec >= time_limit.tv_sec) {
      // Shop closes.
      break;
    }
    
    // Wait for random delay, then create new Customer thread.
    usleep(customer_arrival_time()); 

    // spawn customer threads 
    Customer *cust = new Customer(this, next_customer_id);
    pthread_t* thread = reinterpret_cast<pthread_t*>(malloc(sizeof(pthread_t)));
    
    int rc2 = pthread_create(thread, nullptr,
                              ::run_customer, reinterpret_cast<void *>(cust));
    cout<<"customer " << next_customer_id <<" arrives"<<endl;
    if (rc2 != 0) {
        errno = rc;
        perror("creating pthread");
        exit(EXIT_FAILURE);
    }
  }
  
  //set barbers to closing
  for (auto barber: Shop::barbers) {
    barber->closing_time();
  }
  cout << "the shop closes" << endl;
  
  for (auto thread: barber_threads) {
    pthread_join(*thread, nullptr);
  }
  customers_total = customers_turned_away + customers_served_immediately + customers_waited;
  cout << "customers served immediately: " << customers_served_immediately << endl;
  cout << "customers waited: " << customers_waited << endl;
  cout << "total customers served: " << (customers_served_immediately + customers_waited) << endl;
  cout << "customers turned away: " << customers_turned_away << endl;
  cout << "total customers: " << customers_total << endl;
}

// Customer thread announces arrival to shop. If the collection of
// currently sleeping barbers is not empty, remove and return one
// barber from the collection. If all the barbers are busy and there
// is an empty chair in the waiting room, add the customer to the
// waiting queue and return {nullptr, true}.  Otherwise, the
// customer will leave: return {nullptr, false}.
Shop::BarberOrWait Shop::arrives(Customer* customer) {
  {
    Lock lock(mutex);
    // Find a sleeping barber.
    if (!sleepingBarbers.empty()){
      Barber* barber = sleepingBarbers.front();
      sleepingBarbers.pop();
      customers_served_immediately++;
      return BarberOrWait{barber, true};
    }

    //all barber busy and waiting queue is not full
    if (waitingCustomers.size() < waiting_chairs){
      waitingCustomers.push(customer);
      customers_waited++;
      return BarberOrWait{nullptr, true};
    }

    // Otherwise, customer leaves.
    customers_turned_away++;
    return BarberOrWait{nullptr, false};
  }
}


// Barber thread requests next customer.  If no customers are
// currently waiting, add the barber to the collection of
// currently sleeping barbers and return nullptr.
Customer* Shop::next_customer(Barber* barber) {
  {
    Lock lock(mutex);
    if (!waitingCustomers.empty()){
      Customer* customer = waitingCustomers.front();
      waitingCustomers.pop();
      return customer;
    }
    
    //no customer waiting, add to sleeping barber queue
    sleepingBarbers.push(barber);
    return nullptr;
  }
}


// Return random service time.
int Shop::service_time() {
  // https://www.cplusplus.com/reference/random/normal_distribution/operator()/
  // construct a trivial random generator engine from a time-based seed:
  unsigned seed = chrono::system_clock::now().time_since_epoch().count();
  default_random_engine generator (seed);
  normal_distribution<double> distribution(average_service_time, service_time_deviation);
  int out = distribution(generator);
  if (out < 0.8*average_service_time)
    out = 0.8*average_service_time; //clip at 80% average time
  //cout << "normal "<< out << endl;
  return out*1000; //milliseconds*1000=microsecond
}

// Return random customer arrival.
int Shop::customer_arrival_time() {
  // http://www.cplusplus.com/reference/random/poisson_distribution/operator()/
  // construct a trivial random generator engine from a time-based seed:
  unsigned seed = chrono::system_clock::now().time_since_epoch().count();
  default_random_engine generator (seed);
  poisson_distribution<int> distribution (average_customer_arrival);
  int out = distribution(generator);
  //cout << "poisson "<< out << endl;
  return out*1000; //milliseconds*1000=microsecond
}


Barber::Barber(Shop* shop, int id): id(id), shop(shop){
  closing = false;
  b_mutex = nullptr;
  b_condition = nullptr;
  b_mutex = reinterpret_cast<pthread_mutex_t*>(malloc(sizeof(pthread_mutex_t)));
  pthread_mutex_init(b_mutex, NULL);
  b_condition = reinterpret_cast<pthread_cond_t*>(malloc(sizeof(pthread_cond_t)));
  pthread_cond_init(b_condition, NULL);
}


// Barber thread function.
void Barber::run() {
  //request customer
  Customer *cust = shop->next_customer(this);
  if (cust == nullptr){
      { //barber go to sleep
        Lock lock(b_mutex);
        state = barber_state::SLEEP;
      }
      cout<<"barber "<<this->id<<" takes a nap"<<endl;
  }

  while (true){
    /*
    {
      Lock lock(b_mutex);
      pthread_cond_wait(b_condition, b_mutex);
      
      switch (state){
        case (barber_state::SLEEP):
          {
            //Lock lock(b_mutex);
              if (closing){
                cout<<"barber "<<this->id<<" leaves for home"<<endl;
                return;
              }
              else{
                cout<<"barber "<<this->id<<" takes a nap"<<endl;  
              }
          }
          break;
        case (barber_state::AWAKE):
          cout<<"barber "<<this->id<<" wakes up"<<endl;
          customer->next_customer(this);
          break;
        case (barber_state::SERVICE):
          cout<<"barber "<<this->id<<" gives customer "<<customer->id<<" a hair cut"<<endl;
          customer->finished(); 
          cout<<"barber "<<this->id<<" finishes customer "<<customer->id<<"'s hair cut"<<endl;
          break;
        case (barber_state::ACCEPT):
        cout<<"accpetpayment"<<endl;
          customer->payment_accepted();
          cout<<"barber "<<this->id<<" accepts payment from customer "<<customer->id<<endl;
       
          //request next customer
          Customer* nextCust = shop->next_customer(this);
          if (nextCust == nullptr){  //barber go to sleep 
            {
              //Lock lock(b_mutex);
              if (closing){
                cout<<"barber "<<this->id<<" leaves for home"<<endl;
                return;
              }
              else{
                state = barber_state::SLEEP;
                cout<<"barber "<<this->id<<" takes a nap"<<endl;
              }  
            }
          }
          else{
            {
              //Lock lock(b_mutex);
              customer = nextCust;
              state = barber_state::AWAKE;
              cout<<"barber "<<this->id<<" calls customer "<<customer->id<<endl;
            }
            {
              customer->next_customer(this);
              //Lock lock(customer->c_mutex);
              //customer->state = Customer::customer_state::RECEIVESERVICE;
              //customer->setBarber(this);
              //pthread_cond_signal(customer->c_condition);
            }
          }
          break;
      }
    }*/
    

    {
      Lock lock(b_mutex);
      while (state != barber_state::AWAKE){
        pthread_cond_wait(b_condition, b_mutex); 
        if (closing &&  customer->state == Customer::customer_state::LEAVE){
          cout<<"barber "<<this->id<<" leaves for home"<<endl;
          return;
        }
      }
    }
    customer->next_customer(this);//change customer state to RECEIVESERVICE
    {
      Lock lock(b_mutex);
      while (state != barber_state::SERVICE){
        pthread_cond_wait(b_condition, b_mutex);  
      }
    }
    customer->finished(); //change customer state to PAYMENT
    {
      Lock lock(b_mutex);
      while (state != barber_state::ACCEPT){
        pthread_cond_wait(b_condition, b_mutex); 
      }
    }
    customer->payment_accepted(); // change customer state to LEAVE
    
    //request next customer
    Customer* nextCust = shop->next_customer(this);
      if (nextCust == nullptr){  //barber go to sleep 
        if (closing){
          cout<<"barber "<<this->id<<" leaves for home"<<endl;
          return;
        }
        else{
          Lock lock(b_mutex);
          state = barber_state::SLEEP;
          //customer = nullptr;      
        }
        cout<<"barber "<<this->id<<" takes a nap"<<endl; 
      }
      else{    
        {
          Lock lock(b_mutex);
          customer = nextCust;
        }
        {
          Lock lock(nextCust->c_mutex);
          nextCust->setBarber(this);
          nextCust->state = Customer::customer_state::GOTOBARBER;
          pthread_cond_signal(nextCust->c_condition);
        }
        cout<<"barber "<<this->id<<" calls customer "<<nextCust->id<<endl;
      }
  }
}

// Shop tells barber it's closing time.
void Barber::closing_time() {
  {
    Lock lock(b_mutex);
    closing = true;
    pthread_cond_signal(b_condition);
    //cout<<"----------------------------------->send closing sig barber "<<id<<endl;
  }
}


void Barber::awaken(Customer* customer) {
  {
    Lock lock(b_mutex);
    state = barber_state::AWAKE;
    this->customer = customer;
    pthread_cond_signal(b_condition);
    //cout<<"----------------------------------->send awake sig barber "<<id<<endl;
  }
  cout<<"customer "<<customer->id<<" wakes barber "<<this->id<<endl; 
}


void Barber::customer_sits() {
  {
    Lock lock(b_mutex);
    state = barber_state::SERVICE;
    pthread_cond_signal(b_condition);
    //cout<<"----------------------------------->send service sig barber "<<id<<endl; 
  } 
  cout<<"customer "<<customer->id<<" sits in barber "<<this->id<<"'s chair"<<endl;
}


void Barber::payment() {
  {
    Lock lock(b_mutex);
    state = barber_state::ACCEPT;
    pthread_cond_signal(b_condition);
    //cout<<"----------------------------------->send accept sig barber "<<id<<endl; 
  }
  cout<<"customer "<<customer->id<<" gets up and proffers payment to barber "<<this->id<<endl; 
}


Customer::Customer(Shop* shop, int id): id(id), shop(shop){
  c_mutex = nullptr;
  c_condition = nullptr;
  c_mutex = reinterpret_cast<pthread_mutex_t*>(malloc(sizeof(pthread_mutex_t)));
  pthread_mutex_init(c_mutex, NULL);
  c_condition = reinterpret_cast<pthread_cond_t*>(malloc(sizeof(pthread_cond_t)));
  pthread_cond_init(c_condition, NULL);
}


Customer::~Customer() {
  //deallocate
  pthread_mutex_destroy(c_mutex);
  pthread_cond_destroy(c_condition);
  c_mutex= nullptr;
  c_condition = nullptr;
  delete c_mutex;
  delete c_condition;
}


void Customer::run() {
  //call barber
  Shop::BarberOrWait barberOrWait = shop->arrives(this);
  if (!barberOrWait.chair_available){ //no barber, no wait chair
    cout<<"customer "<<this->id<<" leaves without getting haircut"<<endl;
    return;
  }
  else if (barberOrWait.barber == nullptr){ 
    {  //add to wait queue
      Lock lock(c_mutex);
      Customer::state = Customer::customer_state::WAIT;
    }
    cout<<"customer "<<this->id<<" takes a seat in the waiting room"<<endl;
  }
  else{ //barber available
    {
      Lock lock(c_mutex);
      barber = barberOrWait.barber;
      state = customer_state::GOTOBARBER;
    }
  }
  
  while (true){
    /*{
      Lock lock(c_mutex);
      pthread_cond_wait(c_condition, c_mutex);//cout<<"------------------------------------------>receive customer "<<id<<" sig"<<endl;

      switch (state){
        case (customer_state::WAIT):
          break;
        case (customer_state::GOTOBARBER):
          barber->awaken(this);//barber state to awake
          cout<<"customer "<<id<<" wakes barber "<<barber->id<<endl;
        
          break;  
        case (customer_state::RECEIVESERVICE):
          barber->customer_sits();//barber state to service
          cout<<"customer "<<id<<" sits in barber "<<barber->id<<"'s chair"<<endl;
          break;
        case (customer_state::PAYMENT):
          barber->payment();//barber state to accept payment
          cout<<"customer "<<id<<" gets up and proffers payment to barber "<<barber->id<<endl;
          break;
        case (customer_state::LEAVE):
          cout<<"customer "<<id<<" leaves satisfied"<<endl;
          return;
          //break;  
      }
    }*/
    {
      Lock lock(c_mutex);
      while (state != customer_state::GOTOBARBER){
        pthread_cond_wait(c_condition, c_mutex); 
      } 
    }
    barber->awaken(this);//change barber state to AWAKE
    {
      Lock lock(c_mutex);
      while (state != customer_state::RECEIVESERVICE){
        pthread_cond_wait(c_condition, c_mutex);
      } 
    }
    barber->customer_sits();//change barber state to SERVICE
    {
      Lock lock(c_mutex);
      while (state != customer_state::PAYMENT){
        pthread_cond_wait(c_condition, c_mutex);
      } 
    }
    barber->payment();  //change barber state to ACCEPT
    {
      Lock lock(c_mutex);
      while (state != customer_state::LEAVE){
        pthread_cond_wait(c_condition, c_mutex); 
      }
    }
    cout<<"customer "<<id<<" leaves satisfied"<<endl;    
  }
}


void Customer::next_customer(Barber* barber) {
  {
    Lock lock(this->c_mutex);
    this->barber = barber;
    state = customer_state::RECEIVESERVICE;
    pthread_cond_signal(c_condition);
    //cout<<"----------------------------------->send receiveservice sig customer "<<id<<endl;
  }
  cout<<"barber "<<barber->id<<" wakes up"<<endl;  
}


void Customer::finished() {
  
  usleep(shop->service_time());
  {
    Lock lock(c_mutex);
    state = customer_state::PAYMENT;
    pthread_cond_signal(c_condition);
    //cout<<"----------------------------------->send payment sig customer "<<id<<endl;
  }
  cout<<"barber "<<barber->id<<" gives customer "<<this->id<<" a hair cut"<<endl;
  cout<<"barber "<<barber->id<<" finishes customer "<<this->id<<"'s hair cut"<<endl;
}


void Customer::payment_accepted() {
  {
    Lock lock(c_mutex);
    state = customer_state::LEAVE;
    pthread_cond_signal(c_condition);
    //cout<<"----------------------------------->send leave sig customer "<<id<<endl;
  }
  cout<<"barber "<<barber->id<<" accepts payment from customer "<<this->id<<endl;
}


void usage() {
  cerr
    << "usage: "
    << PROG_NAME
    << " <nbarbers>"
    << " <nchairs>"
    << " <avg_service_time>"
    << " <service_time_std_deviation>"
    << " <avg_customer_arrival_time>"
    << " <duration>"
    << endl;
  exit(EXIT_FAILURE);
}


int main(int argc, char* argv[]) {
  PROG_NAME = argv[0];

  if (argc != 7) {
    usage();
  }
  int barbers = atoi(argv[1]);
  if (barbers <= 0) {
    usage();
  }
  int chairs = atoi(argv[2]);
  if (chairs < 0) {
    usage();
  }
  int service_time = atoi(argv[3]);
  if (service_time <= 0) {
    usage();
  }
  int service_deviation = atoi(argv[4]);
  if (service_time <= 0) {
    usage();
  }
  int customer_arrivals = atoi(argv[5]);
  if (customer_arrivals <= 0) {
    usage();
  }
  int duration = atoi(argv[6]);
  if (duration <= 0) {
    usage();
  }

  Shop barber_shop(barbers,
		   chairs,
		   service_time,
		   service_deviation,
		   customer_arrivals,
		   duration);
  barber_shop.run();

  return EXIT_SUCCESS;
}