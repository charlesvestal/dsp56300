#pragma once
#include <string>
#include <cstdlib>
#include <cstdio>
#include <thread>
#include <chrono>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include <array>
#include <cstring> // memcpy

#include "dsp56kBase/fastmath.h"
#include "dsp56kBase/ringbuffer.h"
#include "utils.h"

namespace dsp56k
{	
	constexpr float g_float2dspScale	= 8388608.0f;
	constexpr float g_dsp2FloatScale	= 1.0f / g_float2dspScale;
	constexpr float g_dspFloatMax		= 8388607.0f;
	constexpr float g_dspFloatMin		= -8388608.0f;

	template<typename T> TWord sample2dsp(T _src)
	{
		return _src;
	}

	template<> inline TWord sample2dsp(float _src)
	{
		_src *= g_float2dspScale;
		_src = clamp(_src, g_dspFloatMin, g_dspFloatMax);

		return floor_int(_src) & 0x00ffffff;
	}

	template<typename T> T dsp2sample(TWord d)
	{
		return d;
	}

	template<> inline float dsp2sample(const TWord d)
	{
		return static_cast<float>(signextend<int32_t,24>(static_cast<int32_t>(d))) * g_dsp2FloatScale;
	}

	class Audio;

	using AudioCallback = std::function<void(Audio*)>;

	class Audio
	{
	public:
		static constexpr uint32_t MaxSlotsPerFrame = 32;
		static constexpr uint32_t TxRegisterCount = 6;
		static constexpr uint32_t RxRegisterCount = 4;

		using TxSlot = std::array<TWord, TxRegisterCount>;
		using RxSlot = std::array<TWord, RxRegisterCount>;

		template<typename TSlot>
		class Frame
		{
		public:
			using Slot = TSlot;
			using Data = std::array<Slot, MaxSlotsPerFrame>;

			Frame() = default;
			~Frame() = default;

			Frame(const Frame&& _source) noexcept : m_slotCount(_source.m_slotCount)
			{
				_source.copyTo(*this);
			}

			Frame(const Frame& _source) : m_slotCount(_source.m_slotCount)
			{
				_source.copyTo(*this);
			}

			Frame& operator = (const Frame& _source)
			{
				_source.copyTo(*this);
				return *this;
			}

			Frame& operator = (Frame&& _source) noexcept
			{
				_source.copyTo(*this);
				return *this;
			}

			const Slot& operator[](size_t _index) const			{ return m_data[_index]; }
			Slot& operator[](size_t _index)						{ return m_data[_index]; }

			[[nodiscard]] uint32_t size() const					{ return m_slotCount; }
			bool empty() const									{ return 0 == size(); }
			void clear()										{ m_slotCount = 0; }
			void resize(const uint32_t _size)					{ m_slotCount = _size; }

			void copyTo(Frame& _target) const
			{
				_target.m_slotCount = m_slotCount;

				::memcpy(_target.m_data.data(), m_data.data(), sizeof(m_data[0]) * m_slotCount);
			}

		private:
			Data m_data;
			uint32_t m_slotCount = 0;
		};

		using TxFrame = Frame<TxSlot>;
		using RxFrame = Frame<RxSlot>;

		// Callbacks receive a frame and a mutable frame index reference.
		// The frame index is owned by Audio and initialized to 0.
		// Callbacks are free to read and modify it to track position.
		using ReadRxCallback = std::function<void(uint64_t&, RxFrame&)>;
		using WriteTxCallback = std::function<void(uint64_t&, const TxFrame&)>;

		explicit Audio(bool _useRingBuffers = true);

		void terminate();

		void setReadRxCallback(ReadRxCallback _callback) { m_readRxCallback = std::move(_callback); }
		void setWriteTxCallback(WriteTxCallback _callback) { m_writeTxCallback = std::move(_callback); }

		bool hasRingBuffers() const { return m_useRingBuffers; }

		// The DSP thread calls the current callback from writeTXimpl while the host thread swaps it
		// here (boot hand-off, terminate()). Assigning the std::function in place let the DSP thread
		// observe it mid-swap and throw bad_function_call, which nothing catches on a DSP thread, so
		// the process aborted. Publish an immutable new callback with one atomic store instead and
		// keep the replaced ones alive - setCallback is a boot/teardown operation, a handful of calls
		// per instance, and only ever from the host/UC thread.
		void setCallback(const AudioCallback& _ac)
		{
			auto cb = std::make_unique<AudioCallback>(_ac ? _ac : AudioCallback([](Audio*) {}));
			auto* const c = cb.get();
			m_callbacks.emplace_back(std::move(cb));
			m_callback.store(c, std::memory_order_release);
		}

		void writeEmptyAudioIn(const size_t _len)
		{
			for (size_t i = 0; i < _len; ++i)
				m_audioInputs.push_back({});
		}

		template<typename T>
		void processAudioInterleaved(const T** _inputs, T** _outputs, const uint32_t _sampleFrames, const size_t _latency = 0)
		{
			processAudioInputInterleaved<T>(_inputs, _sampleFrames, _latency);
			processAudioOutputInterleaved<T>(_outputs, _sampleFrames);
		}
		
		void setMaxInputBacklog(const uint32_t _frames)	{ m_maxInputBacklog = _frames; }
		uint64_t getDroppedInputFrames() const			{ return m_droppedInputFrames.load(std::memory_order_relaxed); }

		/* Longest the CONSUMER may wait for the DSP before giving up on a frame and
		 * treating the rest of the block as silence. 0 disables the bound and restores
		 * the old wait-forever behaviour.
		 *
		 * The default is deliberately generous: 200ms is far beyond any legitimate wait
		 * -- a host at any sane buffer size has long since underrun -- so this never
		 * fires in normal operation and only ever converts "this plugin has wedged the
		 * whole host" into "this plugin glitched". */
		void setMaxOutputWaitUs(const uint32_t _us)		{ m_maxOutputWaitUs = _us; }

		/* Offline (faster-than-real-time) rendering. Both mechanisms below trade audio
		 * for meeting a deadline, and offline there is no deadline to meet: the host
		 * is bouncing or freezing, not playing. Giving up on a frame would put a hole
		 * in the file and discarding the input backlog would drop part of the render,
		 * and since the engine is ALWAYS behind when the host renders as fast as it
		 * can, both would fire on nearly every block. Make the caller wait instead --
		 * it is not a realtime thread. */
		void setNonRealtime(const bool _nonRealtime)	{ m_nonRealtime = _nonRealtime; }
		bool isNonRealtime() const						{ return m_nonRealtime; }

		/* Diagnostic trail for the starvation path, OFF unless built with
		 * -DTUS_AUDIO_HEALTH=1. Deliberately self-contained here rather than routed
		 * through the plugin framework: synthLib::Device exposes no way to reach this
		 * object, and giving jucePluginLib a dsp56kEmu dependency would break the
		 * synths that do not link it.
		 *
		 * ringHeld is the decisive number. Silence with the audio path running on
		 * schedule has two causes that need opposite fixes: starvations climbing with
		 * an EMPTY ring means the producer has died, while no starvations and a
		 * non-empty ring means the producer is alive and handing us zeros -- the
		 * emulated firmware's own audio engine having stalled. */
		/* Says whether the carry bound is engaging. This mechanism is the dsp56k
		 * equivalent of jeThread's m_maxCarrySamples: for these synths the INPUT ring
		 * is the work queue -- a fed input frame obliges the DSP to produce an output
		 * frame -- so unconsumed input frames are the backlog, and skipping them is
		 * "drop it and render NOW".
		 *
		 * It could never fire while the consumer blocked forever in the output wait,
		 * because processBlock() feeds input and reads output in the SAME call: stuck
		 * on output, we never returned to feed input, so the ring never grew past the
		 * bound and the recovery never triggered. Worth knowing whether bounding that
		 * wait has let it start working. */
		void reportCarryDrop(const uint64_t _total, const uint32_t _excess, const size_t _depth, const size_t _limit) const
		{
#if TUS_AUDIO_HEALTH
			if(const auto* home = std::getenv("HOME"))
			{
				const std::string path = std::string(home) + "/Documents/tus_audio_health.log";
				if(auto* f = fopen(path.c_str(), "a"))
				{
					fprintf(f, "  CARRYDROP total=%llu dropped=%u depth=%zu limit=%zu\n",
						static_cast<unsigned long long>(_total), _excess, _depth, _limit);
					fclose(f);
				}
			}
#else
			(void)_total; (void)_excess; (void)_depth; (void)_limit;
#endif
		}

		void reportStarvation(const uint64_t _count, const uint32_t _frame, const uint32_t _frames) const
		{
#if TUS_AUDIO_HEALTH
			// at most one line a second, so a sustained stall cannot flood the file
			const auto now = std::chrono::steady_clock::now();
			if(now - m_lastStarvationReport < std::chrono::seconds(1))
				return;
			m_lastStarvationReport = now;

			if(const auto* home = std::getenv("HOME"))
			{
				const std::string path = std::string(home) + "/Documents/tus_audio_health.log";
				if(auto* f = fopen(path.c_str(), "a"))
				{
					fprintf(f, "  STARVED total=%llu atFrame=%u/%u ringHeld=%zu waitedUs=%u\n",
						static_cast<unsigned long long>(_count), _frame, _frames,
						m_audioOutputs.size(), m_maxOutputWaitUs);
					fclose(f);
				}
			}
#else
			(void)_count; (void)_frame; (void)_frames;
#endif
		}
		uint64_t getOutputStarvations() const			{ return m_outputStarvations.load(std::memory_order_relaxed); }

		template<typename T, typename TFunc>
		void processAudioInput(const uint32_t _frames, const size_t _latency, const TFunc& _createRxFrame)
		{
			/* The legitimate depth of this ring is the configured latency. Beyond
			 * that by more than the bound, the DSP is not merely late, it is not
			 * coming back on its own -- ask it to skip forward. Halve the bound as
			 * the target so a recovery does not sit on the threshold and retrigger. */
			if(m_maxInputBacklog && !m_nonRealtime)
			{
				const auto depth = m_audioInputs.size();
				const auto limit = _latency + m_maxInputBacklog;

				if(depth > limit && !m_discardInputFrames.load(std::memory_order_relaxed))
				{
					const auto excess = static_cast<uint32_t>(depth - (_latency + (m_maxInputBacklog >> 1)));
					m_discardInputFrames.store(excess, std::memory_order_release);
					const auto total = m_droppedInputFrames.fetch_add(excess, std::memory_order_relaxed) + excess;
					reportCarryDrop(total, excess, depth, limit);
				}
			}

			for (uint32_t s = 0; s < _frames; ++s)
			{
				// INPUT

				if(_latency > m_latency)
				{
					// a latency increase on the input means to feed additional zeroes into it
					m_audioInputs.waitNotFull();
					m_audioInputs.push_back({});

					++m_latency;
				}
				if(_latency < m_latency)
				{
					// a latency decrease on the input means to skip writing data
					--m_latency;
				}
				else
				{
					m_audioInputs.emplace_back([&](RxFrame& _frame)
					{
						_createRxFrame(s, _frame);
					});
				}
			}
		}

		template<typename T>
		void processAudioInputInterleaved(const T** _ins, const uint32_t _frames, const size_t _latency = 0)
		{
			return processAudioInput<T>(_frames, _latency, [&](size_t _s, RxFrame& _f)
			{
				_f.resize(2);
				_f[0] = RxSlot{sample2dsp<T>(_ins[0][_s]), sample2dsp<T>(_ins[2][_s]), sample2dsp<T>(_ins[4][_s]), sample2dsp<T>(_ins[6][_s])};
				_f[1] = RxSlot{sample2dsp<T>(_ins[1][_s]), sample2dsp<T>(_ins[3][_s]), sample2dsp<T>(_ins[5][_s]), sample2dsp<T>(_ins[7][_s])};
			});
		}

		template<typename T>
		void processAudioInput(const T* _input, const uint32_t _frames, const uint32_t _slotsPerFrame, const size_t _latency = 0)
		{
			uint32_t readPos = 0;

			return processAudioInput<T>(_frames, _latency, [&](size_t _s, RxFrame& _f)
			{
				_f.resize(_slotsPerFrame);

				for(uint32_t s=0; s<_slotsPerFrame; ++s)
				{
					for(uint32_t i=0; i<_f[s].size(); ++i)
						_f[s][i] = sample2dsp<T>(_input[readPos++]);
				}
			});
		}

		/* Returns the number of frames actually delivered, which may be fewer than
		 * asked for. See setMaxOutputWaitUs(): the caller MUST treat the shortfall as
		 * silence, because this is the consumer side and the caller is, in a plugin,
		 * the host's realtime render thread.
		 *
		 * pop_front() on an audio-mode ring blocks on a semaphore with no timeout
		 * (waitNotEmpty() is a no-op there), so a DSP that cannot keep up used to hold
		 * that thread for as long as it liked. Measured with Osirus as an AUv3 in AUM
		 * on an iPhone 15 Pro, logging wall time per 2s of audio produced: 2000ms while
		 * healthy, then 3399ms, 6086ms, 5767ms once the process had been backgrounded
		 * and throttled -- i.e. sitting in the host's render callback for three times
		 * the block duration, indefinitely. The host cannot service anything else in
		 * that state, so EVERY other plugin went silent too and the only recovery was
		 * killing the host. A struggling synth must degrade alone. */
		template<typename T, typename TFunc>
		uint32_t processAudioOutput(const uint32_t _frames, const TFunc& _readOutputCbk)
		{
			for (uint32_t i = 0; i < _frames; ++i)
			{
				if(m_maxOutputWaitUs && !m_nonRealtime && m_audioOutputs.empty())
				{
					// empty()/size() are lock-free atomic loads, so the starvation check
					// itself never blocks
					const auto deadline = std::chrono::steady_clock::now()
						+ std::chrono::microseconds(m_maxOutputWaitUs);

					while(m_audioOutputs.empty())
					{
						if(std::chrono::steady_clock::now() >= deadline)
						{
							const auto n = m_outputStarvations.fetch_add(1, std::memory_order_relaxed) + 1;
							reportStarvation(n, i, _frames);
							return i;
						}
						std::this_thread::yield();
					}
				}

				m_audioOutputs.waitNotEmpty();
				m_audioOutputs.pop_front([&](TxFrame& _frame)
				{
					_readOutputCbk(i, _frame);
				});
			}
			return _frames;
		}

		template<typename T>
		void processAudioOutputInterleaved(T** _outputs, const uint32_t _sampleFrames)
		{
			const auto delivered = processAudioOutput<T>(_sampleFrames, [&](size_t _frame, TxFrame& _tx)
			{
				if(_tx.empty())
					return;

				_outputs[0 ][_frame] = dsp2sample<T>(_tx[0][0]);
				_outputs[2 ][_frame] = dsp2sample<T>(_tx[0][1]);
				_outputs[4 ][_frame] = dsp2sample<T>(_tx[0][2]);
				_outputs[6 ][_frame] = dsp2sample<T>(_tx[0][3]);
				_outputs[8 ][_frame] = dsp2sample<T>(_tx[0][4]);
				_outputs[10][_frame] = dsp2sample<T>(_tx[0][5]);

				if(_tx.size() < 2)
					return;

				_outputs[ 1][_frame] = dsp2sample<T>(_tx[1][0]);
				_outputs[ 3][_frame] = dsp2sample<T>(_tx[1][1]);
				_outputs[ 5][_frame] = dsp2sample<T>(_tx[1][2]);
				_outputs[ 7][_frame] = dsp2sample<T>(_tx[1][3]);
				_outputs[ 9][_frame] = dsp2sample<T>(_tx[1][4]);
				_outputs[11][_frame] = dsp2sample<T>(_tx[1][5]);
			});

			// the DSP did not deliver in time: the rest of the block is silence. Written
			// explicitly because the caller's buffer may still hold input or stale audio
			for(uint32_t f = delivered; f < _sampleFrames; ++f)
			{
				for(uint32_t ch = 0; ch < 12; ++ch)
					_outputs[ch][f] = T(0);
			}
		}

		template<typename T>
		void processAudioOutput(T* _outputs, const uint32_t _sampleFrames)
		{
			size_t writePos = 0;
			const auto delivered = processAudioOutput<T>(_sampleFrames, [&](size_t _frame, TxFrame& _tx)
			{
				for(size_t s=0; s<_tx.size(); ++s)
				{
					const auto& slot = _tx[s];
					for (const auto v : slot)
						_outputs[writePos++] = dsp2sample<T>(v);
				}
			});

			// shortfall is silence; writePos is where the delivered frames stopped
			if(delivered < _sampleFrames)
			{
				const auto perFrame = delivered ? writePos / delivered : 0;
				for(size_t i = writePos; i < perFrame * _sampleFrames; ++i)
					_outputs[i] = T(0);
			}
		}

		const auto& getAudioInputs() const { return m_audioInputs; }
		const auto& getAudioOutputs() const { return m_audioOutputs; }

		auto& getAudioInputs() { return m_audioInputs; }
		auto& getAudioOutputs() { return m_audioOutputs; }

	public:
		static constexpr uint32_t RingBufferSize = 8192 * 4;

	protected:
		void readRXimpl(RxFrame& _values);
		void writeTXimpl(const TxFrame& _values);

		std::atomic<const AudioCallback*> m_callback;
		std::vector<std::unique_ptr<AudioCallback>> m_callbacks;	// owns every callback ever set, see setCallback

		static void incFrameSync(uint32_t& _frameSync)
		{
			++_frameSync;
			_frameSync &= 1;
		}

		enum FrameSync
		{
			FrameSyncChannelLeft = 1,
			FrameSyncChannelRight = 0
		};

		bool m_useRingBuffers;

		RingBuffer<RxFrame, RingBufferSize, true, false> m_audioInputs;
		RingBuffer<TxFrame, RingBufferSize, true, false> m_audioOutputs;
		size_t m_latency = 0;

		std::atomic<bool> m_terminated{false};

		/* Recovery from a transient overload.
		 *
		 * Host and DSP meet through two rings and BOTH sides block: the DSP waits
		 * in readRX when the input ring is empty, the host waits here when it is
		 * full. So when capacity drops below 1.0x the input ring fills with work
		 * the DSP still owes, up to its 32768-frame capacity -- 0.68 s at 48 kHz.
		 * That backlog is pure latency, and the only party that can retire it is
		 * the DSP, at (capacity - 1.0)x. After a dip that means seconds of lag
		 * that never fully clears, which is heard as the synth "never recovering"
		 * even once the overload is gone.
		 *
		 * The host cannot drop those frames itself: it is the PRODUCER of that
		 * ring, and popping from the producer side of an SPSC queue is a data
		 * race. So it asks instead -- it publishes how many frames to skip, and
		 * the DSP thread discards them on its own side of the ring, where doing
		 * so is safe. One discontinuity, then a stream that is current again. */
		std::atomic<uint32_t>	m_discardInputFrames{0};
		std::atomic<uint64_t>	m_droppedInputFrames{0};	// diagnostics only
		/* OFF unless a device opts in, via setMaxInputBacklog().
		 *
		 * A deep input ring does not mean the same thing on every board. The
		 * NodalRed2x pre-fills this ring on purpose -- writeEmptyAudioIn() plus
		 * its own notify-correction accounting -- so depth there is by design,
		 * not a backlog, and discarding from it starves the ESAI: the DSP spins
		 * forever on btst #$6,x:$ffb3 (SAISR) waiting for a flag that never
		 * comes, while its audio ISR keeps running. That is a HANG, not a
		 * glitch, and enabling this globally caused it. */
		uint32_t				m_maxInputBacklog = 0;
		uint32_t				m_maxOutputWaitUs = 200'000;
		bool					m_nonRealtime = false;
		std::atomic<uint64_t>	m_outputStarvations{0};
		mutable std::chrono::steady_clock::time_point m_lastStarvationReport{};

		ReadRxCallback m_readRxCallback;
		WriteTxCallback m_writeTxCallback;
		uint64_t m_readFrameIndex = 0;
		uint64_t m_writeFrameIndex = 0;
	};
}
