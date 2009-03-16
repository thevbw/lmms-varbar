/*
 * sf2_player.cpp - a soundfont2 player using fluidSynth
 *
 * Copyright (c) 2008 Paul Giblock <drfaygo/at/gmail/dot/com>
 * Copyright (c) 2009 Tobias Doerffel <tobydox/at/users.sourceforge.net>
 * 
 * This file is part of Linux MultiMedia Studio - http://lmms.sourceforge.net
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 */

#include <QtCore/QDebug>
#include <QtCore/QTextStream>
#include <QtGui/QLayout>
#include <QtGui/QLabel>
#include <QtGui/QFileDialog>
#include <QtXml/QDomDocument>

#include "sf2_player.h"
#include "engine.h"
#include "instrument_track.h"
#include "instrument_play_handle.h"
#include "note_play_handle.h"
#include "knob.h"
#include "song.h"

#include "main_window.h"
#include "patches_dialog.h"
#include "tooltip.h"
#include "lcd_spinbox.h"

#undef SINGLE_SOURCE_COMPILE
#include "embed.cpp"


extern "C"
{

plugin::descriptor sf2player_plugin_descriptor =
{
	STRINGIFY( PLUGIN_NAME ),
	"Sf2 Player",
	QT_TRANSLATE_NOOP( "pluginBrowser",
			"Player for SoundFont files" ),
	"Paul Giblock <drfaygo/at/gmail/dot/com>",
	0x0100,
	plugin::Instrument,
	new pluginPixmapLoader( "logo" ),
	"sf2",
	NULL
} ;

}


// Static map of current sfonts
QMap<QString, sf2Font*> sf2Instrument::s_fonts;
QMutex sf2Instrument::s_fontsMutex;



sf2Instrument::sf2Instrument( instrumentTrack * _instrument_track ) :
	instrument( _instrument_track, &sf2player_plugin_descriptor ),
	m_srcState( NULL ),
	m_font( NULL ),
	m_fontId( 0 ),
	m_filename( "" ),
	m_lastMidiPitch( 8192 ),
	m_bankNum( 0, 0, 999, this, tr("Bank") ),
	m_patchNum( 0, 0, 127, this, tr("Patch") ),
	m_gain( 1.0f, 0.0f, 5.0f, 0.01f, this, tr( "Gain" ) ),
	m_reverbOn( FALSE, this, tr( "Reverb" ) ),
	m_reverbRoomSize( FLUID_REVERB_DEFAULT_ROOMSIZE, 0, 1.0, 0.01f, 
			this, tr( "Reverb Roomsize" ) ),
	m_reverbDamping( FLUID_REVERB_DEFAULT_DAMP, 0, 1.0, 0.01, 
			this, tr( "Reverb Damping" ) ),
	m_reverbWidth( FLUID_REVERB_DEFAULT_WIDTH, 0, 1.0, 0.01f, 
			this, tr( "Reverb Width" ) ),
	m_reverbLevel( FLUID_REVERB_DEFAULT_LEVEL, 0, 1.0, 0.01f, 
			this, tr( "Reverb Level" ) ),
	m_chorusOn( FALSE, this, tr( "Chorus" ) ),
	m_chorusNum( FLUID_CHORUS_DEFAULT_N, 0, 10.0, 1.0, 
			this, tr( "Chorus Lines" ) ),
	m_chorusLevel( FLUID_CHORUS_DEFAULT_LEVEL, 0, 10.0, 0.01, 
			this, tr( "Chorus Level" ) ),
	m_chorusSpeed( FLUID_CHORUS_DEFAULT_SPEED, 0.29, 5.0, 0.01, 
			this, tr( "Chorus Speed" ) ),
	m_chorusDepth( FLUID_CHORUS_DEFAULT_DEPTH, 0, 46.0, 0.05, 
			this, tr( "Chorus Depth" ) )
{
	for( int i = 0; i < 128; ++i )
	{
		m_notesRunning[i] = 0;
	}

	m_settings = new_fluid_settings();

	fluid_settings_setint( m_settings, (char *) "audio.period-size",
					engine::getMixer()->framesPerPeriod() );

	// This is just our starting instance of synth.  It is recreated
	// everytime we load a new soundfont.
	m_synth = new_fluid_synth( m_settings );

	instrumentPlayHandle * iph = new instrumentPlayHandle( this );
	engine::getMixer()->addPlayHandle( iph );

	//loadFile( configManager::inst()->defaultSoundfont() );

	updateSampleRate();
	updateReverbOn();
	updateReverb();
	updateChorusOn();
	updateChorus();
	updateGain();

	
	connect( &m_bankNum, SIGNAL( dataChanged() ),
			this, SLOT( updatePatch() ) );

	connect( &m_patchNum, SIGNAL( dataChanged() ),
			this, SLOT( updatePatch() ) );
	
	connect( engine::getMixer(), SIGNAL( sampleRateChanged() ),
			this, SLOT( updateSampleRate() ) );

	// Gain
	connect( &m_gain, SIGNAL( dataChanged() ),
			this, SLOT( updateGain() ) );

	// Reverb
	connect( &m_reverbOn, SIGNAL( dataChanged() ),
			this, SLOT( updateReverbOn() ) );

	connect( &m_reverbRoomSize, SIGNAL( dataChanged() ),
			this, SLOT( updateReverb() ) );

	connect( &m_reverbDamping, SIGNAL( dataChanged() ),
			this, SLOT( updateReverb() ) );

	connect( &m_reverbWidth, SIGNAL( dataChanged() ),
			this, SLOT( updateReverb() ) );

	connect( &m_reverbLevel, SIGNAL( dataChanged() ),
			this, SLOT( updateReverb() ) );

	// Chorus
	connect( &m_chorusOn, SIGNAL( dataChanged() ),
			this, SLOT( updateChorusOn() ) );

	connect( &m_chorusNum, SIGNAL( dataChanged() ),
			this, SLOT( updateChorus() ) );

	connect( &m_chorusLevel, SIGNAL( dataChanged() ),
			this, SLOT( updateChorus() ) );

	connect( &m_chorusSpeed, SIGNAL( dataChanged() ),
			this, SLOT( updateChorus() ) );

	connect( &m_chorusDepth, SIGNAL( dataChanged() ),
			this, SLOT( updateChorus() ) );

}



sf2Instrument::~sf2Instrument()
{
	engine::getMixer()->removePlayHandles( getInstrumentTrack() );
	freeFont();
	delete_fluid_synth( m_synth );
	delete_fluid_settings( m_settings );
	if( m_srcState != NULL )
	{
		src_delete( m_srcState );
	}

}



void sf2Instrument::saveSettings( QDomDocument & _doc, QDomElement & _this )
{
	_this.setAttribute( "src", m_filename );
	m_patchNum.saveSettings( _doc, _this, "patch" );
	m_bankNum.saveSettings( _doc, _this, "bank" );

	m_gain.saveSettings( _doc, _this, "gain" );

	m_reverbOn.saveSettings( _doc, _this, "reverbOn" );
	m_reverbRoomSize.saveSettings( _doc, _this, "reverbRoomSize" );
	m_reverbDamping.saveSettings( _doc, _this, "reverbDamping" );
	m_reverbWidth.saveSettings( _doc, _this, "reverbWidth" );
	m_reverbLevel.saveSettings( _doc, _this, "reverbLevel" );

	m_chorusOn.saveSettings( _doc, _this, "chorusOn" );
	m_chorusNum.saveSettings( _doc, _this, "chorusNum" );
	m_chorusLevel.saveSettings( _doc, _this, "chorusLevel" );
	m_chorusSpeed.saveSettings( _doc, _this, "chorusSpeed" );
	m_chorusDepth.saveSettings( _doc, _this, "chorusDepth" );
}




void sf2Instrument::loadSettings( const QDomElement & _this )
{
	openFile( _this.attribute( "src" ) );
	m_patchNum.loadSettings( _this, "patch" );
	m_bankNum.loadSettings( _this, "bank" );

	m_gain.loadSettings( _this, "gain" );

	m_reverbOn.loadSettings( _this, "reverbOn" );
	m_reverbRoomSize.loadSettings( _this, "reverbRoomSize" );
	m_reverbDamping.loadSettings( _this, "reverbDamping" );
	m_reverbWidth.loadSettings( _this, "reverbWidth" );
	m_reverbLevel.loadSettings( _this, "reverbLevel" );

	m_chorusOn.loadSettings( _this, "chorusOn" );
	m_chorusNum.loadSettings( _this, "chorusNum" );
	m_chorusLevel.loadSettings( _this, "chorusLevel" );
	m_chorusSpeed.loadSettings( _this, "chorusSpeed" );
	m_chorusDepth.loadSettings( _this, "chorusDepth" );

	updatePatch();
	updateGain();
}




void sf2Instrument::loadFile( const QString & _file )
{
	if( !_file.isEmpty() )
	{
		openFile( _file );
		updatePatch();

		// for some reason we've to call that, otherwise preview of a
		// soundfont for the first time fails
		updateSampleRate();
	}
}




automatableModel * sf2Instrument::getChildModel( const QString & _modelName )
{
	if( _modelName == "bank" )
	{
		return &m_bankNum;
	}
	else if( _modelName == "patch" )
	{
		return &m_patchNum;
	}
	qCritical() << "requested unknown model " << _modelName;
	return NULL;
}



QString sf2Instrument::nodeName( void ) const
{
	return( sf2player_plugin_descriptor.name );
}




void sf2Instrument::freeFont( void )
{
	QTextStream cout( stdout, QIODevice::WriteOnly );

	m_synthMutex.lock();
	
	if ( m_font != NULL )
	{
		s_fontsMutex.lock();
		--(m_font->refCount);
	
		// No more references
		if( m_font->refCount <= 0 )
		{
			cout << "Really deleting " << m_filename << endl;

			fluid_synth_sfunload( m_synth, m_fontId, TRUE );
			s_fonts.remove( m_filename );
			delete m_font;
		}
		// Just remove our reference
		else
		{
			cout << "un-referencing " << m_filename << endl;

			fluid_synth_remove_sfont( m_synth, m_font->fluidFont );
		}
		s_fontsMutex.unlock();

		m_font = NULL;
	}
	m_synthMutex.unlock();
}



void sf2Instrument::openFile( const QString & _sf2File )
{
	emit fileLoading();

	// Used for loading file
	char * sf2Ascii = qstrdup( qPrintable(
			sampleBuffer::tryToMakeAbsolute( _sf2File ) ) );
	QString relativePath = sampleBuffer::tryToMakeRelative( _sf2File );

	// free reference to soundfont if one is selected
	freeFont();

	m_synthMutex.lock();
	s_fontsMutex.lock();

	// Increment Reference
	if( s_fonts.contains( relativePath ) )
	{
		QTextStream cout( stdout, QIODevice::WriteOnly );
		cout << "Using existing reference to " << relativePath << endl;

		m_font = s_fonts[ relativePath ];

		m_font->refCount++;

		m_fontId = fluid_synth_add_sfont( m_synth, m_font->fluidFont );		
	}

	// Add to map, if doesn't exist.  
	else
	{
		m_fontId = fluid_synth_sfload( m_synth, sf2Ascii, TRUE );

		if( fluid_synth_sfcount( m_synth ) > 0 )
		{
			// Grab this sf from the top of the stack and add to list
			m_font = new sf2Font( fluid_synth_get_sfont( m_synth, 0 ) );
			s_fonts.insert( relativePath, m_font );
		}
		else
		{
			// TODO: Couldn't load file!
		}
	}

	s_fontsMutex.unlock();
	m_synthMutex.unlock();

	if( m_fontId >= 0 )
	{
		// Don't reset patch/bank, so that it isn't cleared when
		// someone resolves a missing file
		//m_patchNum.setValue( 0 );
		//m_bankNum.setValue( 0 );
		m_filename = relativePath;

		emit fileChanged();
	}

	delete[] sf2Ascii;
}




void sf2Instrument::updatePatch( void )
{
	if( m_bankNum.value() >= 0 && m_patchNum.value() >= 0 )
	{
		fluid_synth_program_select( m_synth, 1, m_fontId,
				m_bankNum.value(), m_patchNum.value() );
	}
}




QString sf2Instrument::getCurrentPatchName( void )
{
	int iBankSelected = m_bankNum.value();
	int iProgSelected = m_patchNum.value();

	fluid_preset_t preset;
	// For all soundfonts (in reversed stack order) fill the available programs...
	int cSoundFonts = ::fluid_synth_sfcount( m_synth );
	for( int i = 0; i < cSoundFonts; i++ )
	{
		fluid_sfont_t *pSoundFont = fluid_synth_get_sfont( m_synth, i );
		if (pSoundFont) {
#ifdef CONFIG_FLUID_BANK_OFFSET
			int iBankOffset =
				fluid_synth_get_bank_offset(
						m_synth, pSoundFont->id );
#endif
			pSoundFont->iteration_start( pSoundFont );
			while( pSoundFont->iteration_next( pSoundFont,
								&preset ) )
			{
				int iBank = preset.get_banknum( &preset );
#ifdef CONFIG_FLUID_BANK_OFFSET
				iBank += iBankOffset;
#endif
				int iProg = preset.get_num( &preset );
				if( iBank == iBankSelected && iProg ==
								iProgSelected )
				{
					return preset.get_name( &preset );
				}
			}
		}
	}
	return "";
}




void sf2Instrument::updateGain( void )
{
	fluid_synth_set_gain( m_synth, m_gain.value() );
}




void sf2Instrument::updateReverbOn( void )
{
	fluid_synth_set_reverb_on( m_synth, m_reverbOn.value() ? 1 : 0 );
}




void sf2Instrument::updateReverb( void )
{
	fluid_synth_set_reverb( m_synth, m_reverbRoomSize.value(),
			m_reverbDamping.value(), m_reverbWidth.value(),
			m_reverbLevel.value() );
}




void  sf2Instrument::updateChorusOn( void )
{
	fluid_synth_set_chorus_on( m_synth, m_chorusOn.value() ? 1 : 0 );
}




void  sf2Instrument::updateChorus( void )
{
	fluid_synth_set_chorus( m_synth, static_cast<int>( m_chorusNum.value() ),
			m_chorusLevel.value(), m_chorusSpeed.value(),
			m_chorusDepth.value(), 0 );
}



void sf2Instrument::updateSampleRate( void )
{	
	double tempRate;
	
	// Set & get, returns the true sample rate
	fluid_settings_setnum( m_settings, (char *) "synth.sample-rate",
				engine::getMixer()->processingSampleRate() );
	fluid_settings_getnum( m_settings, (char *) "synth.sample-rate",
								&tempRate );
	m_internalSampleRate = static_cast<int>( tempRate );

	if( m_font )
	{
		// Now, delete the old one and replace
		m_synthMutex.lock();
		fluid_synth_remove_sfont( m_synth, m_font->fluidFont ); 
		delete_fluid_synth( m_synth );

		// New synth
		m_synth = new_fluid_synth( m_settings );
		m_fontId = fluid_synth_add_sfont( m_synth, m_font->fluidFont );		
		m_synthMutex.unlock();

		// synth program change (set bank and patch)
		updatePatch();
		updateGain();
	}
	else
	{
		// Recreate synth with no soundfonts
		m_synthMutex.lock();
		delete_fluid_synth( m_synth );
		m_synth = new_fluid_synth( m_settings );
		m_synthMutex.unlock();
	}

	m_synthMutex.lock();
	if( engine::getMixer()->currentQualitySettings().interpolation >=
			mixer::qualitySettings::Interpolation_SincFastest )
	{
		fluid_synth_set_interp_method( m_synth, -1,
							FLUID_INTERP_7THORDER );
	}
	else
	{
		fluid_synth_set_interp_method( m_synth, -1,
							FLUID_INTERP_DEFAULT );
	}
	m_synthMutex.unlock();
	if( m_internalSampleRate < engine::getMixer()->processingSampleRate() )
	{
		m_synthMutex.lock();
		if( m_srcState != NULL )
		{
			src_delete( m_srcState );
		}
		int error;
		m_srcState = src_new( engine::getMixer()->
				currentQualitySettings().libsrcInterpolation(),
					DEFAULT_CHANNELS, &error );
		if( m_srcState == NULL || error )
		{
			printf( "error while creating SRC-data-"
				"structure in sf2Instrument::"
				"updateSampleRate()\n" );
		}
		m_synthMutex.unlock();
	}
	updateReverb();
	updateChorus();
}




void sf2Instrument::playNote( notePlayHandle * _n, sampleFrame * )
{
	const float LOG440 = 2.643452676f;

	const f_cnt_t tfp = _n->totalFramesPlayed();

	int midiNote = (int)floor( 12.0 * ( log2( _n->unpitchedFrequency() ) -
							LOG440 ) - 4.0 );

	// out of range?
	if( midiNote <= 0 || midiNote >= 128 )
	{
		return;
	}

	if( tfp == 0 )
	{
		_n->m_pluginData = new int( midiNote );

		m_synthMutex.lock();
		fluid_synth_noteon( m_synth, 1, midiNote,
							_n->getMidiVelocity() );
		m_synthMutex.unlock();

		m_notesRunningMutex.lock();
		++m_notesRunning[midiNote];
		m_notesRunningMutex.unlock();
	}
}


// Could we get iph-based instruments support sample-exact models by using a 
// frame-length of 1 while rendering?
void sf2Instrument::play( sampleFrame * _working_buffer )
{
	const fpp_t frames = engine::getMixer()->framesPerPeriod();

	m_synthMutex.lock();
	if( m_lastMidiPitch != getInstrumentTrack()->midiPitch() )
	{
		m_lastMidiPitch = getInstrumentTrack()->midiPitch();
		fluid_synth_pitch_bend( m_synth, 1, m_lastMidiPitch );
	}

	if( m_internalSampleRate < engine::getMixer()->processingSampleRate() &&
							m_srcState != NULL )
	{
		const fpp_t f = frames * m_internalSampleRate /
				engine::getMixer()->processingSampleRate();
		sampleFrame * tmp = new sampleFrame[f];
		fluid_synth_write_float( m_synth, f, tmp, 0, 2, tmp, 1, 2 );

		SRC_DATA src_data;
		src_data.data_in = tmp[0];
		src_data.data_out = _working_buffer[0];
		src_data.input_frames = f;
		src_data.output_frames = frames;
		src_data.src_ratio = (double) frames / f;
		src_data.end_of_input = 0;
		int error = src_process( m_srcState, &src_data );
		delete[] tmp;
		if( error )
		{
			printf( "sf2Instrument: error while resampling: %s\n",
							src_strerror( error ) );
		}
		if( src_data.output_frames_gen > frames )
		{
			printf( "sf2Instrument: not enough frames: %ld / %d\n",
					src_data.output_frames_gen, frames );
		}
	}
	else
	{
		fluid_synth_write_float( m_synth, frames, _working_buffer, 0, 2,
							_working_buffer, 1, 2 );
	}
	m_synthMutex.unlock();

	getInstrumentTrack()->processAudioBuffer( _working_buffer, frames,
									NULL );
}




void sf2Instrument::deleteNotePluginData( notePlayHandle * _n )
{
	int * midiNote = static_cast<int *>( _n->m_pluginData );
	m_notesRunningMutex.lock();
	const int n = --m_notesRunning[*midiNote];
	m_notesRunningMutex.unlock();
	if( n <= 0 )
	{
		m_synthMutex.lock();
		fluid_synth_noteoff( m_synth, 1, *midiNote );
		m_synthMutex.unlock();
	}

	delete midiNote;
}




pluginView * sf2Instrument::instantiateView( QWidget * _parent )
{
	return( new sf2InstrumentView( this, _parent ) );
}







class sf2Knob : public knob
{
public:
	sf2Knob( QWidget * _parent ) :
			knob( knobStyled, _parent )
	{
		setFixedSize( 31, 38 );
	}
};



sf2InstrumentView::sf2InstrumentView( instrument * _instrument,
			QWidget * _parent ) :
	instrumentView( _instrument, _parent )
{
//	QVBoxLayout * vl = new QVBoxLayout( this );
//	QHBoxLayout * hl = new QHBoxLayout();
	
	sf2Instrument * k = castModel<sf2Instrument>();
	connect( &k->m_bankNum, SIGNAL( dataChanged() ),
			this, SLOT( updatePatchName() ) );

	connect( &k->m_patchNum, SIGNAL( dataChanged() ),
			this, SLOT( updatePatchName() ) );
	
	// File Button
	m_fileDialogButton = new pixmapButton( this );
	m_fileDialogButton->setCursor( QCursor( Qt::PointingHandCursor ) );
	m_fileDialogButton->setActiveGraphic( PLUGIN_NAME::getIconPixmap(
			"fileselect_on" ) );
	m_fileDialogButton->setInactiveGraphic( PLUGIN_NAME::getIconPixmap(
			"fileselect_off" ) );
	m_fileDialogButton->move( 217, 107 );
	connect( m_fileDialogButton, SIGNAL( clicked() ),
			this, SLOT( showFileDialog() ) );
	toolTip::add( m_fileDialogButton, tr( "Open other SoundFont file" ) );

	m_fileDialogButton->setWhatsThis(
			tr( "Click here to open another SF2 file" ) );

	// Patch Button
	m_patchDialogButton = new pixmapButton( this );
	m_patchDialogButton->setCursor( QCursor( Qt::PointingHandCursor ) );
	m_patchDialogButton->setActiveGraphic( PLUGIN_NAME::getIconPixmap(
							"patches_on" ) );
	m_patchDialogButton->setInactiveGraphic( PLUGIN_NAME::getIconPixmap(
							"patches_off" ) );
	m_patchDialogButton->setEnabled( FALSE );
	m_patchDialogButton->move( 217, 125 );

	connect( m_patchDialogButton, SIGNAL( clicked() ),
			this, SLOT( showPatchDialog() ) );
	toolTip::add( m_patchDialogButton, tr( "Choose the patch" ) );


	// LCDs
	m_bankNumLcd = new lcdSpinBox( 3, "21pink", this );
	m_bankNumLcd->move(131, 62);
//	m_bankNumLcd->addTextForValue( -1, "---" );
//	m_bankNumLcd->setEnabled( FALSE );

	m_patchNumLcd = new lcdSpinBox( 3, "21pink", this );
	m_patchNumLcd->move(190, 62);
//	m_patchNumLcd->addTextForValue( -1, "---" );
//	m_patchNumLcd->setEnabled( FALSE );

	/*hl->addWidget( m_fileDialogButton );
	hl->addWidget( m_bankNumLcd );
	hl->addWidget( m_patchNumLcd );
	hl->addWidget( m_patchDialogButton );

	vl->addLayout( hl );*/

	// Next row

	//hl = new QHBoxLayout();

	m_filenameLabel = new QLabel( this );
	m_filenameLabel->setGeometry( 58, 109, 156, 11 );
	m_patchLabel = new QLabel( this );
	m_patchLabel->setGeometry( 58, 127, 156, 11 );

	//hl->addWidget( m_filenameLabel );
//	vl->addLayout( hl );

	// Gain
	m_gainKnob = new sf2Knob( this );
	m_gainKnob->setHintText( tr("Gain") + " ", "" );
	m_gainKnob->move( 86, 55 );
//	vl->addWidget( m_gainKnob );

	// Reverb
//	hl = new QHBoxLayout();

	
	m_reverbButton = new pixmapButton( this );
	m_reverbButton->setCheckable( TRUE );
	m_reverbButton->move( 24, 176 );
	m_reverbButton->setActiveGraphic( PLUGIN_NAME::getIconPixmap(
							"reverb_on" ) );
	m_reverbButton->setInactiveGraphic( PLUGIN_NAME::getIconPixmap(
							"reverb_off" ) );
	toolTip::add( m_reverbButton, tr( "Apply reverb (if supported)" ) );
	m_reverbButton->setWhatsThis(
		tr( "This button enables the reverb effect. "
			"This is useful for cool effects, but only works on "
			"files that support it." ) );


	m_reverbRoomSizeKnob = new sf2Knob( this );
	m_reverbRoomSizeKnob->setHintText( tr("Reverb Roomsize:") + " ", "" );
	m_reverbRoomSizeKnob->move( 93, 160 );

	m_reverbDampingKnob = new sf2Knob( this );
	m_reverbDampingKnob->setHintText( tr("Reverb Damping:") + " ", "" );
	m_reverbDampingKnob->move( 130, 160 );

	m_reverbWidthKnob = new sf2Knob( this );
	m_reverbWidthKnob->setHintText( tr("Reverb Width:") + " ", "" );
	m_reverbWidthKnob->move( 167, 160 );

	m_reverbLevelKnob = new sf2Knob( this );
	m_reverbLevelKnob->setHintText( tr("Reverb Level:") + " ", "" );
	m_reverbLevelKnob->move( 204, 160 );

/*	hl->addWidget( m_reverbOnLed );
	hl->addWidget( m_reverbRoomSizeKnob );
	hl->addWidget( m_reverbDampingKnob );
	hl->addWidget( m_reverbWidthKnob );
	hl->addWidget( m_reverbLevelKnob );

	vl->addLayout( hl );
*/

	// Chorus
//	hl = new QHBoxLayout();

	m_chorusButton = new pixmapButton( this );
	m_chorusButton->setCheckable( TRUE );
	m_chorusButton->move( 24, 222 );
	m_chorusButton->setActiveGraphic( PLUGIN_NAME::getIconPixmap(
							"chorus_on" ) );
	m_chorusButton->setInactiveGraphic( PLUGIN_NAME::getIconPixmap(
							"chorus_off" ) );
	toolTip::add( m_reverbButton, tr( "Apply chorus (if supported)" ) );
	m_chorusButton->setWhatsThis(
		tr( "This button enables the chorus effect. "
			"This is useful for cool echo effects, but only works on "
			"files that support it." ) );

	m_chorusNumKnob = new sf2Knob( this );
	m_chorusNumKnob->setHintText( tr("Chorus Lines:") + " ", "" );
	m_chorusNumKnob->move( 93, 206 );

	m_chorusLevelKnob = new sf2Knob( this );
	m_chorusLevelKnob->setHintText( tr("Chorus Level:") + " ", "" );
	m_chorusLevelKnob->move( 130 , 206 );

	m_chorusSpeedKnob = new sf2Knob( this );
	m_chorusSpeedKnob->setHintText( tr("Chorus Speed:") + " ", "" );
	m_chorusSpeedKnob->move( 167 , 206 );

	m_chorusDepthKnob = new sf2Knob( this );
	m_chorusDepthKnob->setHintText( tr("Chorus Depth:") + " ", "" );
	m_chorusDepthKnob->move( 204 , 206 );
/*
	hl->addWidget( m_chorusOnLed );
	hl->addWidget( m_chorusNumKnob);
	hl->addWidget( m_chorusLevelKnob);
	hl->addWidget( m_chorusSpeedKnob);
	hl->addWidget( m_chorusDepthKnob);

	vl->addLayout( hl );
*/
	setAutoFillBackground( TRUE );
	QPalette pal;
	pal.setBrush( backgroundRole(), PLUGIN_NAME::getIconPixmap(
								"artwork" ) );
	setPalette( pal );

	updateFilename();

}




sf2InstrumentView::~sf2InstrumentView()
{
}




void sf2InstrumentView::modelChanged( void )
{
	sf2Instrument * k = castModel<sf2Instrument>();
	m_bankNumLcd->setModel( &k->m_bankNum );
	m_patchNumLcd->setModel( &k->m_patchNum );
	
	m_gainKnob->setModel( &k->m_gain );

	m_reverbButton->setModel( &k->m_reverbOn );
	m_reverbRoomSizeKnob->setModel( &k->m_reverbRoomSize );
	m_reverbDampingKnob->setModel( &k->m_reverbDamping );
	m_reverbWidthKnob->setModel( &k->m_reverbWidth );
	m_reverbLevelKnob->setModel( &k->m_reverbLevel );

	m_chorusButton->setModel( &k->m_chorusOn );
	m_chorusNumKnob->setModel( &k->m_chorusNum );
	m_chorusLevelKnob->setModel( &k->m_chorusLevel );
	m_chorusSpeedKnob->setModel( &k->m_chorusSpeed );
	m_chorusDepthKnob->setModel( &k->m_chorusDepth );


	connect(k, SIGNAL( fileChanged( void ) ),
			this, SLOT( updateFilename( void ) ) );

	connect(k, SIGNAL( fileLoading( void ) ),
			this, SLOT( invalidateFile( void ) ) );

	updateFilename();

}




void sf2InstrumentView::updateFilename( void )
{
	sf2Instrument * i = castModel<sf2Instrument>();
	QFontMetrics fm( m_filenameLabel->font() );
	QString file = i->m_filename.endsWith( ".sf2", Qt::CaseInsensitive ) ?
			i->m_filename.left( i->m_filename.length() - 4 ) :
			i->m_filename;
	m_filenameLabel->setText(
			fm.elidedText( file, Qt::ElideLeft, 
			m_filenameLabel->width() ) );
			//		i->m_filename + "\nPatch: TODO" );

	m_patchDialogButton->setEnabled( !i->m_filename.isEmpty() );

	updatePatchName();

	update();
}




void sf2InstrumentView::updatePatchName( void )
{
	sf2Instrument * i = castModel<sf2Instrument>();
	QFontMetrics fm( font() );
	QString patch = i->getCurrentPatchName();
	m_patchLabel->setText(
			fm.elidedText( patch, Qt::ElideLeft, 
			m_patchLabel->width() ) );


	update();
}




void sf2InstrumentView::invalidateFile( void )
{
	m_patchDialogButton->setEnabled( false );
}




void sf2InstrumentView::showFileDialog( void )
{
	sf2Instrument * k = castModel<sf2Instrument>();

	QFileDialog ofd( NULL, tr( "Open SoundFont file" ) );
	ofd.setFileMode( QFileDialog::ExistingFiles );

	QStringList types;
	types << tr( "SoundFont2 Files (*.sf2)" );
	ofd.setFilters( types );

	QString dir;
	if( k->m_filename != "" )
	{
		QString f = k->m_filename;
		if( QFileInfo( f ).isRelative() )
		{
			f = configManager::inst()->userSamplesDir() + f;
			if( QFileInfo( f ).exists() == FALSE )
			{
				f = configManager::inst()->factorySamplesDir() +
						k->m_filename;
			}
		}
		ofd.setDirectory( QFileInfo( f ).absolutePath() );
		ofd.selectFile( QFileInfo( f ).fileName() );
	}
	else
	{
		ofd.setDirectory( configManager::inst()->userSamplesDir() );
	}

	m_fileDialogButton->setEnabled( false );
	
	if( ofd.exec() == QDialog::Accepted && !ofd.selectedFiles().isEmpty() )
	{
		QString f = ofd.selectedFiles()[0];
		if( f != "" )
		{
			k->openFile( f );
			engine::getSong()->setModified();
		}
	}

	m_fileDialogButton->setEnabled( true );
}




void sf2InstrumentView::showPatchDialog( void )
{
	sf2Instrument * k = castModel<sf2Instrument>();

	patchesDialog pd( this );

	pd.setup( k->m_synth, 1, k->getInstrumentTrack()->name(),
		&k->m_bankNum, &k->m_patchNum, m_patchLabel );

	pd.exec();
}



extern "C"
{

// neccessary for getting instance out of shared lib
plugin * lmms_plugin_main( model *, void * _data )
{
	return( new sf2Instrument(
			static_cast<instrumentTrack *>( _data ) ) );
}


}

#include "moc_sf2_player.cxx"

