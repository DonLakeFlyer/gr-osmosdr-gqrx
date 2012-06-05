/* -*- c++ -*- */
/*
 * Copyright 2012 Dimitri Stolnikov <horiz0n@gmx.net>
 *
 * GNU Radio is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * GNU Radio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Radio; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

/*
 * config.h is generated by configure.  It contains the results
 * of probing for features, options etc.  It should be the first
 * file included in your .cc file.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "osmosdr_src_c.h"
#include <gr_io_signature.h>

#include <boost/assign.hpp>
#include <boost/format.hpp>

#include <stdexcept>
#include <iostream>
#include <stdio.h>

#include <osmosdr.h>

#include <osmosdr_arg_helpers.h>

using namespace boost::assign;

#define BUF_SIZE  (16 * 32 * 512)
#define BUF_NUM   32
#define BUF_SKIP  1 // buffers to skip due to garbage

#define BYTES_PER_SAMPLE  4 // osmosdr device delivers 16 bit signed IQ data

/*
 * Create a new instance of osmosdr_src_c and return
 * a boost shared_ptr.  This is effectively the public constructor.
 */
osmosdr_src_c_sptr
osmosdr_make_src_c (const std::string &args)
{
  return gnuradio::get_initial_sptr(new osmosdr_src_c (args));
}

/*
 * The private constructor
 */
osmosdr_src_c::osmosdr_src_c (const std::string &args)
  : gr_sync_block ("osmosdr_src_c",
        gr_make_io_signature (0, 0, sizeof (gr_complex)),
        args_to_io_signature(args)),
    _running(true),
    _auto_gain(false),
    _skipped(0)
{
  int ret;
  unsigned int dev_index = 0, mcr = 0;
  size_t nchan = 1;

  dict_t dict = params_to_dict(args);

  if (dict.count("osmosdr"))
    dev_index = boost::lexical_cast< unsigned int >( dict["osmosdr"] );

  if (dict.count("mcr"))
    mcr = (unsigned int) boost::lexical_cast< double >( dict["mcr"] );

  if (mcr != 0)
    throw std::runtime_error("FIXME: Setting the MCR is not supported.");

  if (dict.count("nchan"))
    nchan = boost::lexical_cast< size_t >( dict["nchan"] );

  if (nchan != 1)
    throw std::runtime_error("FIXME: Values of nchan != 1 are not supported.");

  _buf_num = BUF_NUM;
  _buf_head = _buf_used = _buf_offset = 0;
  _samp_avail = BUF_SIZE / BYTES_PER_SAMPLE;

  if (dict.count("buffers")) {
    _buf_num = (unsigned int)boost::lexical_cast< double >( dict["buffers"] );
    if (0 == _buf_num)
      _buf_num = BUF_NUM;
    std::cerr << "Using " << _buf_num << " buffers of size " << BUF_SIZE << "."
              << std::endl;
  }

  if ( dev_index >= osmosdr_get_device_count() )
    throw std::runtime_error("Wrong osmosdr device index given.");

  std::cerr << "Using device #" << dev_index << ": "
            << osmosdr_get_device_name(dev_index)
            << std::endl;

  _dev = NULL;
  ret = osmosdr_open( &_dev, dev_index );
  if (ret < 0)
    throw std::runtime_error("Failed to open osmosdr device.");

  ret = osmosdr_set_fpga_iq_swap(_dev, 0);
  if (ret < 0)
    throw std::runtime_error("Failed to disable IQ swapping.");

  ret = osmosdr_set_sample_rate( _dev, 500000 );
  if (ret < 0)
    throw std::runtime_error("Failed to set default samplerate.");

  ret = osmosdr_set_tuner_gain_mode(_dev, int(!_auto_gain));
  if (ret < 0)
    throw std::runtime_error("Failed to enable manual gain mode.");

  ret = osmosdr_reset_buffer( _dev );
  if (ret < 0)
    throw std::runtime_error("Failed to reset usb buffers.");

  _buf = (unsigned short **) malloc(_buf_num * sizeof(unsigned short *));

  for(unsigned int i = 0; i < _buf_num; ++i)
    _buf[i] = (unsigned short *) malloc(BUF_SIZE);

  _thread = gruel::thread(_osmosdr_wait, this);
}

/*
 * Our virtual destructor.
 */
osmosdr_src_c::~osmosdr_src_c ()
{
  if (_dev) {
    _running = false;
    osmosdr_cancel_async( _dev );
    _thread.timed_join( boost::posix_time::milliseconds(200) );
    osmosdr_close( _dev );
    _dev = NULL;
  }

  for(unsigned int i = 0; i < _buf_num; ++i) {
    if (_buf[i])
      free(_buf[i]);
  }

  free(_buf);
  _buf = NULL;
}

void osmosdr_src_c::_osmosdr_callback(unsigned char *buf, uint32_t len, void *ctx)
{
  osmosdr_src_c *obj = (osmosdr_src_c *)ctx;
  obj->osmosdr_callback(buf, len);
}

void osmosdr_src_c::osmosdr_callback(unsigned char *buf, uint32_t len)
{
  if (_skipped < BUF_SKIP) {
    _skipped++;
    return;
  }

  {
    boost::mutex::scoped_lock lock( _buf_mutex );

    int buf_tail = (_buf_head + _buf_used) % _buf_num;
    memcpy(_buf[buf_tail], buf, len);

    if (_buf_used == _buf_num) {
      printf("O"); fflush(stdout);
      _buf_head = (_buf_head + 1) % _buf_num;
    } else {
      _buf_used++;
    }
  }

  _buf_cond.notify_one();
}

void osmosdr_src_c::_osmosdr_wait(osmosdr_src_c *obj)
{
  obj->osmosdr_wait();
}

void osmosdr_src_c::osmosdr_wait()
{
  int ret = osmosdr_read_async( _dev, _osmosdr_callback, (void *)this, 0, BUF_SIZE );

  _running = false;

  if ( ret != 0 )
    std::cerr << "osmosdr_read_async returned with " << ret << std::endl;
}

int osmosdr_src_c::work( int noutput_items,
                        gr_vector_const_void_star &input_items,
                        gr_vector_void_star &output_items )
{
  gr_complex *out = (gr_complex *)output_items[0];

  {
    boost::mutex::scoped_lock lock( _buf_mutex );

    while (_buf_used < 3 && _running) // collect at least 3 buffers
      _buf_cond.wait( lock );
  }

  if (!_running)
    return WORK_DONE;

  short *buf = (short *)_buf[_buf_head] + _buf_offset;

  if (noutput_items <= _samp_avail) {
    for (int i = 0; i < noutput_items; i++)
       *out++ = gr_complex( float(*(buf + i * 2 + 0)) * (1.0f/32767.5f),
                            float(*(buf + i * 2 + 1)) * (1.0f/32767.5f) );

    _buf_offset += noutput_items * 2;
    _samp_avail -= noutput_items;
  } else {
    for (int i = 0; i < _samp_avail; i++)
      *out++ = gr_complex( float(*(buf + i * 2 + 0)) * (1.0f/32767.5f),
                           float(*(buf + i * 2 + 1)) * (1.0f/32767.5f) );

    {
      boost::mutex::scoped_lock lock( _buf_mutex );

      _buf_head = (_buf_head + 1) % _buf_num;
      _buf_used--;
    }

    buf = (short *)_buf[_buf_head];

    int remaining = noutput_items - _samp_avail;

    for (int i = 0; i < remaining; i++)
      *out++ = gr_complex( float(*(buf + i * 2 + 0)) * (1.0f/32767.5f),
                           float(*(buf + i * 2 + 1)) * (1.0f/32767.5f) );

    _buf_offset = remaining * 2;
    _samp_avail = (BUF_SIZE / BYTES_PER_SAMPLE) - remaining;
  }

  return noutput_items;
}

std::vector<std::string> osmosdr_src_c::get_devices()
{
  std::vector< std::string > devices;

  for (unsigned int i = 0; i < osmosdr_get_device_count(); i++) {
    std::string args = "osmosdr=" + boost::lexical_cast< std::string >( i );
    args += ",label='" + std::string(osmosdr_get_device_name( i )) + "'";
    devices.push_back( args );
  }

  return devices;
}

size_t osmosdr_src_c::get_num_channels()
{
  return 1;
}

osmosdr::meta_range_t osmosdr_src_c::get_sample_rates()
{
  osmosdr::meta_range_t range;

  if (_dev) {
    int count = osmosdr_get_sample_rates(_dev, NULL);
    if (count > 0) {
      uint32_t* rates = new uint32_t[ count ];
      count = osmosdr_get_sample_rates(_dev, rates);
      for (int i = 0; i < count; i++)
        range += osmosdr::range_t( rates[i] );
      delete[] rates;
    }
  }

  return range;
}

double osmosdr_src_c::set_sample_rate(double rate)
{
  if (_dev) {
    osmosdr_set_sample_rate( _dev, (uint32_t)rate );
  }

  return get_sample_rate();
}

double osmosdr_src_c::get_sample_rate()
{
  if (_dev)
    return (double)osmosdr_get_sample_rate( _dev );

  return 0;
}

osmosdr::freq_range_t osmosdr_src_c::get_freq_range( size_t chan )
{
  osmosdr::freq_range_t range;

  /* there is a (temperature dependent) gap between 1100 to 1250 MHz */
  range += osmosdr::range_t( 50e6, 2.2e9, 100 );

  return range;
}

double osmosdr_src_c::set_center_freq( double freq, size_t chan )
{
  if (_dev)
    osmosdr_set_center_freq( _dev, (uint32_t)freq );

  return get_center_freq( chan );
}

double osmosdr_src_c::get_center_freq( size_t chan )
{
  if (_dev)
    return (double)osmosdr_get_center_freq( _dev );

  return 0;
}

double osmosdr_src_c::set_freq_corr( double ppm, size_t chan )
{
  return get_freq_corr( chan );
}

double osmosdr_src_c::get_freq_corr( size_t chan )
{
  return 0;
}

std::vector<std::string> osmosdr_src_c::get_gain_names( size_t chan )
{
  std::vector< std::string > gains;

  gains += "LNA";

  return gains;
}

osmosdr::gain_range_t osmosdr_src_c::get_gain_range( size_t chan )
{
  osmosdr::gain_range_t range;

  if (_dev) {
    int count = osmosdr_get_tuner_gains(_dev, NULL);
    if (count > 0) {
      int* gains = new int[ count ];
      count = osmosdr_get_tuner_gains(_dev, gains);
      for (int i = 0; i < count; i++)
        range += osmosdr::range_t( gains[i] / 10.0 );
      delete[] gains;
    }
  }

  return range;
}

osmosdr::gain_range_t osmosdr_src_c::get_gain_range( const std::string & name, size_t chan )
{
  return get_gain_range( chan );
}

bool osmosdr_src_c::set_gain_mode( bool automatic, size_t chan )
{
  if (_dev) {
    if (!osmosdr_set_tuner_gain_mode(_dev, int(!automatic))) {
      _auto_gain = automatic;
    }
  }

  return get_gain_mode(chan);
}

bool osmosdr_src_c::get_gain_mode( size_t chan )
{
  return _auto_gain;
}

static double pick_closest_gain(osmosdr::gain_range_t &gains, double required)
{
  double result = required;
  double distance = 100;

  BOOST_FOREACH(osmosdr::range_t gain, gains)
  {
    double diff = fabs(gain.start() - required);

    if (diff < distance) {
      distance = diff;
      result = gain.start();
    }
  }

  return result;
}

double osmosdr_src_c::set_gain( double gain, size_t chan )
{
  osmosdr::gain_range_t gains = osmosdr_src_c::get_gain_range( chan );
  double picked_gain = pick_closest_gain( gains, gain );

  if (_dev)
    osmosdr_set_tuner_gain( _dev, int(picked_gain * 10.0) );

  return get_gain( chan );
}

double osmosdr_src_c::set_gain( double gain, const std::string & name, size_t chan)
{
  return set_gain( gain, chan );
}

double osmosdr_src_c::get_gain( size_t chan )
{
  if ( _dev )
    return ((double)osmosdr_get_tuner_gain( _dev )) / 10.0;

  return 0;
}

double osmosdr_src_c::get_gain( const std::string & name, size_t chan )
{
  return get_gain( chan );
}

std::vector< std::string > osmosdr_src_c::get_antennas( size_t chan )
{
  std::vector< std::string > antennas;

  antennas += get_antenna( chan );

  return antennas;
}

std::string osmosdr_src_c::set_antenna( const std::string & antenna, size_t chan )
{
  return get_antenna( chan );
}

std::string osmosdr_src_c::get_antenna( size_t chan )
{
  return "ANT";
}
