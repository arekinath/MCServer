
// Ravines.cpp

// Implements the cStructGenRavines class representing the ravine structure generator

#include "Globals.h"
#include "Ravines.h"




/// How many ravines in each direction are generated for a given chunk. Must be an even number
static const int NEIGHBORHOOD_SIZE = 8;

static const int NUM_RAVINE_POINTS = 4;





struct cRavDefPoint
{
	int m_BlockX;
	int m_BlockZ;
	int m_Radius;
	int m_Top;
	int m_Bottom;
	
	cRavDefPoint(int a_BlockX, int a_BlockZ, int a_Radius, int a_Top, int a_Bottom) :
		m_BlockX(a_BlockX),
		m_BlockZ(a_BlockZ),
		m_Radius(a_Radius),
		m_Top   (a_Top),
		m_Bottom(a_Bottom)
	{
	}
} ;

typedef std::vector<cRavDefPoint> cRavDefPoints;





class cStructGenRavines::cRavine
{
	cRavDefPoints m_Points;
	
	/// Generates the shaping defpoints for the ravine, based on the ravine block coords and noise
	void GenerateBaseDefPoints(int a_BlockX, int a_BlockZ, int a_Size, cNoise & a_Noise);
	
	/// Refines (adds and smooths) defpoints from a_Src into a_Dst
	void RefineDefPoints(const cRavDefPoints & a_Src, cRavDefPoints & a_Dst);
	
	/// Does one round of smoothing, two passes of RefineDefPoints()
	void Smooth(void);
	
	/// Linearly interpolates the points so that the maximum distance between two neighbors is max 1 block
	void FinishLinear(void);
	
public:
	// Coords for which the ravine was generated (not necessarily the center)
	int m_BlockX;
	int m_BlockZ;
	
	cRavine(int a_BlockX, int a_BlockZ, int a_Size, cNoise & a_Noise);
	
	/// Carves the ravine into the chunk specified
	void ProcessChunk(
		int a_ChunkX, int a_ChunkZ, 
		cChunkDef::BlockTypes & a_BlockTypes, 
		cChunkDef::HeightMap & a_HeightMap
	);
	
	#ifdef _DEBUG
	/// Exports itself as a SVG line definition
	AString ExportAsSVG(int a_Color, int a_OffsetX = 0, int a_OffsetZ = 0) const;
	#endif  // _DEBUG
} ;





///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// cStructGenRavines:

cStructGenRavines::cStructGenRavines(int a_Seed, int a_Size) :
	m_Noise(a_Seed),
	m_Size(a_Size)
{
}





cStructGenRavines::~cStructGenRavines()
{
	ClearCache();
}





void cStructGenRavines::ClearCache(void)
{
	for (cRavines::const_iterator itr = m_Cache.begin(), end = m_Cache.end(); itr != end; ++itr)
	{
		delete *itr;
	}  // for itr - m_Cache[]
	m_Cache.clear();
}





void cStructGenRavines::GenStructures(cChunkDesc & a_ChunkDesc)
{
	int ChunkX = a_ChunkDesc.GetChunkX();
	int ChunkZ = a_ChunkDesc.GetChunkZ();
	cRavines Ravines;
	GetRavinesForChunk(ChunkX, ChunkZ, Ravines);
	for (cRavines::const_iterator itr = Ravines.begin(), end = Ravines.end(); itr != end; ++itr)
	{
		(*itr)->ProcessChunk(ChunkX, ChunkZ, a_ChunkDesc.GetBlockTypes(), a_ChunkDesc.GetHeightMap());
	}  // for itr - Ravines[]
}





void cStructGenRavines::GetRavinesForChunk(int a_ChunkX, int a_ChunkZ, cStructGenRavines::cRavines & a_Ravines)
{
	int BaseX = a_ChunkX * cChunkDef::Width / m_Size;
	int BaseZ = a_ChunkZ * cChunkDef::Width / m_Size;
	if (BaseX < 0)
	{
		--BaseX;
	}
	if (BaseZ < 0)
	{
		--BaseZ;
	}
	BaseX -= 4;
	BaseZ -= 4;
	
	// Walk the cache, move each ravine that we want into a_Ravines:
	int StartX = BaseX * m_Size;
	int EndX = (BaseX + NEIGHBORHOOD_SIZE + 1) * m_Size;
	int StartZ = BaseZ * m_Size;
	int EndZ = (BaseZ + NEIGHBORHOOD_SIZE + 1) * m_Size;
	for (cRavines::iterator itr = m_Cache.begin(), end = m_Cache.end(); itr != end;)
	{
		if (
			((*itr)->m_BlockX >= StartX) && ((*itr)->m_BlockX < EndX) &&
			((*itr)->m_BlockZ >= StartZ) && ((*itr)->m_BlockZ < EndZ)
		)
		{
			// want
			a_Ravines.push_back(*itr);
			itr = m_Cache.erase(itr);
		}
		else
		{
			// don't want
			++itr;
		}
	}  // for itr - m_Cache[]
	
	for (int x = 0; x < NEIGHBORHOOD_SIZE; x++)
	{
		int RealX = (BaseX + x) * m_Size;
		for (int z = 0; z < NEIGHBORHOOD_SIZE; z++)
		{
			int RealZ = (BaseZ + z) * m_Size;
			bool Found = false;
			for (cRavines::const_iterator itr = a_Ravines.begin(), end = a_Ravines.end(); itr != end; ++itr)
			{
				if (((*itr)->m_BlockX == RealX) && ((*itr)->m_BlockZ == RealZ))
				{
					Found = true;
					break;
				}
			}
			if (!Found)
			{
				a_Ravines.push_back(new cRavine(RealX, RealZ, m_Size, m_Noise));
			}
		}
	}
	
	// Copy a_Ravines into m_Cache to the beginning:
	cRavines RavinesCopy(a_Ravines);
	m_Cache.splice(m_Cache.begin(), RavinesCopy, RavinesCopy.begin(), RavinesCopy.end());
	
	// Trim the cache if it's too long:
	if (m_Cache.size() > 100)
	{
		cRavines::iterator itr = m_Cache.begin();
		std::advance(itr, 100);
		for (cRavines::iterator end = m_Cache.end(); itr != end; ++itr)
		{
			delete *itr;
		}
		itr = m_Cache.begin();
		std::advance(itr, 100);
		m_Cache.erase(itr, m_Cache.end());
	}
	
	/*
	#ifdef _DEBUG
	// DEBUG: Export as SVG into a file specific for the chunk, for visual verification:
	AString SVG;
	SVG.append("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1024\" height = \"1024\">\n");
	for (cRavines::const_iterator itr = a_Ravines.begin(), end = a_Ravines.end(); itr != end; ++itr)
	{
		SVG.append((*itr)->ExportAsSVG(0, 512, 512));
	}
	SVG.append("</svg>\n");
	
	AString fnam;
	Printf(fnam, "ravines\\%03d_%03d.svg", a_ChunkX, a_ChunkZ);
	cFile File(fnam, cFile::fmWrite);
	File.Write(SVG.c_str(), SVG.size());
	#endif  // _DEBUG
	//*/
}






///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// cStructGenRavines::cRavine

cStructGenRavines::cRavine::cRavine(int a_BlockX, int a_BlockZ, int a_Size, cNoise & a_Noise) :
	m_BlockX(a_BlockX),
	m_BlockZ(a_BlockZ)
{
	// Calculate the ravine shape-defining points:
	GenerateBaseDefPoints(a_BlockX, a_BlockZ, a_Size, a_Noise);
	
	// Smooth the ravine. A two passes are needed:
	Smooth();
	Smooth();
	
	// Linearly interpolate the neighbors so that they're close enough together:
	FinishLinear();
}





void cStructGenRavines::cRavine::GenerateBaseDefPoints(int a_BlockX, int a_BlockZ, int a_Size, cNoise & a_Noise)
{
	// Modify the size slightly to have different-sized ravines (1/2 to 1/1 of a_Size):
	a_Size = (512 + ((a_Noise.IntNoise3DInt(19 * a_BlockX, 11 * a_BlockZ, a_BlockX + a_BlockZ) / 17) % 512)) * a_Size / 1024;
	
	// The complete offset of the ravine from its cellpoint, up to 2 * a_Size in each direction
	int OffsetX = (((a_Noise.IntNoise3DInt(50 * a_BlockX, 30 * a_BlockZ, 0)    / 9) % (2 * a_Size)) + ((a_Noise.IntNoise3DInt(30 * a_BlockX, 50 * m_BlockZ, 1000) / 7) % (2 * a_Size)) - 2 * a_Size) / 2;
	int OffsetZ = (((a_Noise.IntNoise3DInt(50 * a_BlockX, 30 * a_BlockZ, 2000) / 7) % (2 * a_Size)) + ((a_Noise.IntNoise3DInt(30 * a_BlockX, 50 * m_BlockZ, 3000) / 9) % (2 * a_Size)) - 2 * a_Size) / 2;
	int CenterX = a_BlockX + OffsetX;
	int CenterZ = a_BlockZ + OffsetZ;
	
	// Get the base angle in which the ravine "axis" goes:
	float Angle = (float)(((float)((a_Noise.IntNoise3DInt(20 * a_BlockX, 70 * a_BlockZ, 6000) / 9) % 16384)) / 16384.0 * 3.141592653);
	float xc = sin(Angle);
	float zc = cos(Angle);
	
	// Calculate the definition points and radii:
	int MaxRadius = (int)(sqrt(12.0 + ((a_Noise.IntNoise2DInt(61 * a_BlockX, 97 * a_BlockZ) / 13) % a_Size) / 16));
	int Top       = 32 + ((a_Noise.IntNoise2DInt(13 * a_BlockX, 17 * a_BlockZ) / 23) % 32);
	int Bottom    = 5 + ((a_Noise.IntNoise2DInt(17 * a_BlockX, 29 * a_BlockZ) / 13) % 32);
	int Mid = (Top + Bottom) / 2;
	int PointX = CenterX - (int)(xc * a_Size / 2);
	int PointZ = CenterZ - (int)(zc * a_Size / 2);
	m_Points.push_back(cRavDefPoint(PointX, PointZ, 0, (Mid + Top) / 2, (Mid + Bottom) / 2));
	for (int i = 1; i < NUM_RAVINE_POINTS - 1; i++)
	{
		int LineX = CenterX + (int)(xc * a_Size * (i - NUM_RAVINE_POINTS / 2) / NUM_RAVINE_POINTS);
		int LineZ = CenterZ + (int)(zc * a_Size * (i - NUM_RAVINE_POINTS / 2) / NUM_RAVINE_POINTS);
		// Amplitude is the amount of blocks that this point is away from the ravine "axis"
		int Amplitude = (a_Noise.IntNoise3DInt(70 * a_BlockX, 20 * a_BlockZ + 31 * i, 10000 * i) / 9) % a_Size;
		Amplitude = Amplitude / 4 - a_Size / 8;  // Amplitude is in interval [-a_Size / 4, a_Size / 4]
		int PointX = LineX + (int)(zc * Amplitude);
		int PointZ = LineZ - (int)(xc * Amplitude);
		int Radius = MaxRadius - abs(i - NUM_RAVINE_POINTS / 2);  // TODO: better radius function
		int ThisTop    = Top    + ((a_Noise.IntNoise3DInt(7 *  a_BlockX, 19 * a_BlockZ, i * 31) / 13) % 8) - 4;
		int ThisBottom = Bottom + ((a_Noise.IntNoise3DInt(19 * a_BlockX, 7 *  a_BlockZ, i * 31) / 13) % 8) - 4;
		m_Points.push_back(cRavDefPoint(PointX, PointZ, Radius, ThisTop, ThisBottom));
	}  // for i - m_Points[]
	PointX = CenterX + (int)(xc * a_Size / 2);
	PointZ = CenterZ + (int)(zc * a_Size / 2);
	m_Points.push_back(cRavDefPoint(PointX, PointZ, 0, Mid, Mid));
}





void cStructGenRavines::cRavine::RefineDefPoints(const cRavDefPoints & a_Src, cRavDefPoints & a_Dst)
{
	// Smoothing: for each line segment, add points on its 1/4 lengths
	int Num = a_Src.size() - 2;  // this many intermediary points
	a_Dst.clear();
	a_Dst.reserve(Num * 2 + 2);
	cRavDefPoints::const_iterator itr = a_Src.begin() + 1;
	a_Dst.push_back(a_Src.front());
	int PrevX = a_Src.front().m_BlockX;
	int PrevZ = a_Src.front().m_BlockZ;
	int PrevR = a_Src.front().m_Radius;
	int PrevT = a_Src.front().m_Top;
	int PrevB = a_Src.front().m_Bottom;
	for (int i = 0; i <= Num; ++i, ++itr)
	{
		int dx = itr->m_BlockX - PrevX;
		int dz = itr->m_BlockZ - PrevZ;
		if (abs(dx) + abs(dz) < 4)
		{
			// Too short a segment to smooth-subdivide into quarters
			continue;
		}
		int dr = itr->m_Radius - PrevR;
		int dt = itr->m_Top    - PrevT;
		int db = itr->m_Bottom - PrevB;
		int Rad1 = std::max(PrevR + 1 * dr / 4, 1);
		int Rad2 = std::max(PrevR + 3 * dr / 4, 1);
		a_Dst.push_back(cRavDefPoint(PrevX + 1 * dx / 4, PrevZ + 1 * dz / 4, Rad1, PrevT + 1 * dt / 4, PrevB + 1 * db / 4));
		a_Dst.push_back(cRavDefPoint(PrevX + 3 * dx / 4, PrevZ + 3 * dz / 4, Rad2, PrevT + 3 * dt / 4, PrevB + 3 * db / 4));
		PrevX = itr->m_BlockX;
		PrevZ = itr->m_BlockZ;
		PrevR = itr->m_Radius;
		PrevT = itr->m_Top;
		PrevB = itr->m_Bottom;
	}
	a_Dst.push_back(a_Src.back());
}





void cStructGenRavines::cRavine::Smooth(void)
{
	cRavDefPoints Pts;
	RefineDefPoints(m_Points, Pts);  // Refine m_Points -> Pts
	RefineDefPoints(Pts, m_Points);  // Refine Pts -> m_Points
}





void cStructGenRavines::cRavine::FinishLinear(void)
{
	// For each segment, use Bresenham's line algorithm to draw a "line" of defpoints
	// _X 2012_07_20: I tried modifying this algorithm to produce "thick" lines (only one coord change per point)
	//   But the results were about the same as the original, so I disposed of it again - no need to use twice the count of points
	
	cRavDefPoints Pts;
	std::swap(Pts, m_Points);
	
	m_Points.reserve(Pts.size() * 3);
	int PrevX = Pts.front().m_BlockX;
	int PrevZ = Pts.front().m_BlockZ;
	for (cRavDefPoints::const_iterator itr = Pts.begin() + 1, end = Pts.end(); itr != end; ++itr)
	{
		int x1 = itr->m_BlockX;
		int z1 = itr->m_BlockZ;
		int dx = abs(x1 - PrevX);
		int dz = abs(z1 - PrevZ);
		int sx = (PrevX < x1) ? 1 : -1;
		int sz = (PrevZ < z1) ? 1 : -1;
		int err = dx - dz;
		int R = itr->m_Radius;
		int T = itr->m_Top;
		int B = itr->m_Bottom;
		for (;;)
		{
			m_Points.push_back(cRavDefPoint(PrevX, PrevZ, R, T, B));
			if ((PrevX == x1) && (PrevZ == z1))
			{
				break;
			}
			int e2 = 2 * err;
			if (e2 > -dz)
			{
				err -= dz;
				PrevX += sx;
			}
			if (e2 < dx)
			{
				err += dx;
				PrevZ += sz;
			}
		}  // while (true)
	}  // for itr
}





#ifdef _DEBUG
AString cStructGenRavines::cRavine::ExportAsSVG(int a_Color, int a_OffsetX, int a_OffsetZ) const
{
	AString SVG;
  AppendPrintf(SVG, "<path style=\"fill:none;stroke:#%06x;stroke-width:1px;\"\nd=\"", a_Color);
  char Prefix = 'M';  // The first point needs "M" prefix, all the others need "L"
  for (cRavDefPoints::const_iterator itr = m_Points.begin(); itr != m_Points.end(); ++itr)
  {
		AppendPrintf(SVG, "%c %d,%d ", Prefix, a_OffsetX + itr->m_BlockX, a_OffsetZ + itr->m_BlockZ);
		Prefix = 'L';
  }
  SVG.append("\"/>\n");
  
  // Base point highlight:
  AppendPrintf(SVG, "<path style=\"fill:none;stroke:#ff0000;stroke-width:1px;\"\nd=\"M %d,%d L %d,%d\"/>\n",
		a_OffsetX + m_BlockX - 5, a_OffsetZ + m_BlockZ, a_OffsetX + m_BlockX + 5, a_OffsetZ + m_BlockZ
	);
  AppendPrintf(SVG, "<path style=\"fill:none;stroke:#ff0000;stroke-width:1px;\"\nd=\"M %d,%d L %d,%d\"/>\n",
		a_OffsetX + m_BlockX, a_OffsetZ + m_BlockZ - 5, a_OffsetX + m_BlockX, a_OffsetZ + m_BlockZ + 5
	);
	
	// A gray line from the base point to the first point of the ravine, for identification:
  AppendPrintf(SVG, "<path style=\"fill:none;stroke:#cfcfcf;stroke-width:1px;\"\nd=\"M %d,%d L %d,%d\"/>\n",
		a_OffsetX + m_BlockX, a_OffsetZ + m_BlockZ, a_OffsetX + m_Points.front().m_BlockX, a_OffsetZ + m_Points.front().m_BlockZ
	);
	
	// Offset guides:
	if (a_OffsetX > 0)
	{
		AppendPrintf(SVG, "<path style=\"fill:none;stroke:#0000ff;stroke-width:1px;\"\nd=\"M %d,0 L %d,1024\"/>\n",
			a_OffsetX, a_OffsetX
		);
	}
	if (a_OffsetZ > 0)
	{
		AppendPrintf(SVG, "<path style=\"fill:none;stroke:#0000ff;stroke-width:1px;\"\nd=\"M 0,%d L 1024,%d\"/>\n",
			a_OffsetZ, a_OffsetZ
		);
	}
	return SVG;
}
#endif  // _DEBUG





void cStructGenRavines::cRavine::ProcessChunk(
	int a_ChunkX, int a_ChunkZ, 
	cChunkDef::BlockTypes & a_BlockTypes,
	cChunkDef::HeightMap & a_HeightMap
)
{
	int BlockStartX = a_ChunkX * cChunkDef::Width;
	int BlockStartZ = a_ChunkZ * cChunkDef::Width;
	int BlockEndX = BlockStartX + cChunkDef::Width;
	int BlockEndZ = BlockStartZ + cChunkDef::Width;
	for (cRavDefPoints::const_iterator itr = m_Points.begin(), end = m_Points.end(); itr != end; ++itr)
	{
		if (
			(itr->m_BlockX + itr->m_Radius < BlockStartX) ||
			(itr->m_BlockX - itr->m_Radius > BlockEndX) ||
			(itr->m_BlockZ + itr->m_Radius < BlockStartZ) ||
			(itr->m_BlockZ - itr->m_Radius > BlockEndZ)
		)
		{
			// Cannot intersect, bail out early
			continue;
		}
		
		// Carve out a cylinder around the xz point, m_Radius in diameter, from Bottom to Top:
		int RadiusSq = itr->m_Radius * itr->m_Radius;  // instead of doing sqrt for each distance, we do sqr of the radius
		int DifX = BlockStartX - itr->m_BlockX;  // substitution for faster calc
		int DifZ = BlockStartZ - itr->m_BlockZ;  // substitution for faster calc
		for (int x = 0; x < cChunkDef::Width; x++) for (int z = 0; z < cChunkDef::Width; z++)
		{
			#ifdef _DEBUG
			// DEBUG: Make the ravine shapepoints visible on a single layer (so that we can see with Minutor what's going on)
			if ((DifX + x == 0) && (DifZ + z == 0))
			{
				cChunkDef::SetBlock(a_BlockTypes, x, 4, z, E_BLOCK_LAPIS_ORE);
			}
			#endif  // _DEBUG
			
			int DistSq = (DifX + x) * (DifX + x) + (DifZ + z) * (DifZ + z);
			if (DistSq <= RadiusSq)
			{
				int Top = std::min(itr->m_Top, (int)(cChunkDef::Height));  // Stupid gcc needs int cast
				for (int y = std::max(itr->m_Bottom, 1); y <= Top; y++)
				{
					switch (cChunkDef::GetBlock(a_BlockTypes, x, y, z))
					{
						// Only carve out these specific block types
						case E_BLOCK_DIRT:
						case E_BLOCK_GRASS:
						case E_BLOCK_STONE:
						case E_BLOCK_COBBLESTONE:
						case E_BLOCK_GRAVEL:
						case E_BLOCK_SAND:
						case E_BLOCK_SANDSTONE:
						case E_BLOCK_NETHERRACK:
						case E_BLOCK_COAL_ORE:
						case E_BLOCK_IRON_ORE:
						case E_BLOCK_GOLD_ORE:
						case E_BLOCK_DIAMOND_ORE:
						case E_BLOCK_REDSTONE_ORE:
						case E_BLOCK_REDSTONE_ORE_GLOWING:
						{
							cChunkDef::SetBlock(a_BlockTypes, x, y, z, E_BLOCK_AIR);
							break;
						}
						default: break;
					}
				}
			}
		}  // for x, z - a_BlockTypes
	}  // for itr - m_Points[]
}




